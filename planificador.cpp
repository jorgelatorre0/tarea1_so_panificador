#define _POSIX_C_SOURCE 200809L

#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <csignal>

using namespace std;

//Constantes y estructura de datos

//Largo maximo del ID de una actividad
#define max_id_len 64
//Largo maximo del nombre de una actividad
#define max_nombre_len 128
//Maximo de dependencias que puede tener una actividad
#define max_deps 20
// Mximo de actividades que pueden depender de una sola
#define max_dependientes 1000
//Maximo de actividades del plan (10000+1 de margen)
#define max_actividades 10001
//Tiempo minimo al asignar duracion aleatoria
#define tiempo_min_ms 100
//Tiempo maximo al asignar duracion aleatoria
#define tiempo_max_ms 5000

//Estados por lo que pasa una actividad
enum EstadoActividad {Pendiente, Lista, Ejecutando, Ok, Fallida, Abortada};

//Informacion de una actividad del plan
struct Actividad {
    char id[max_id_len];
    char nombre[max_nombre_len];
    int  tiempo_ms;
    char dep_ids[max_deps][max_id_len];
    int  num_deps;
    int  dep_idx[max_deps];
    int  dependientes_idx[max_dependientes];
    int  num_dependientes;
    int fd_escritura[max_dependientes];
    int fd_lectura[max_deps];
    int num_fd_lectura; 
    EstadoActividad estado;
    pid_t pid;
};

//Variables globales para que el manejador de señales pueda acceder al plan

// Arreglo con todas las actividades
static Actividad g_actividades[max_actividades];
//Cuantas actividades se cargaron
static int g_num_actividades = 0;


//Funciones manuales axiliares

//Compara dos textos caracter por caracter, devuelve true si son iguales
static bool son_iguales(const char *s1, const char *s2){
    //Avanza mientras ninguno de los dos haya terminado
    while(*s1 != '\0' && *s2 != '\0'){
        if(*s1 != *s2)
          return false;
        s1++; 
        s2++;
    }
    return *s1 == *s2;
}

//Quita espacios, tabs y saltos de linea del inicio y del final
static string trim(string s){
    int inicio = 0;
    int fin = s.length() - 1;
    //Salta espacios del inicio
    while(inicio <= fin && (s[inicio] == ' ' || s[inicio] == '\t' || s[inicio] == '\n' ||s[inicio] == '\r')){
        inicio++;
    }
    //Retrocede espacios del final
    while(fin >= inicio && (s[fin] == ' ' || s[fin] == '\t' || s[fin] == '\n' || s[fin] == '\r')){
        fin--;
    }
  //Copiamos manualmente caracter por caracter a un nuevo string
    string resultado = "";
    for(int i = inicio; i <= fin; i++){
        resultado += s[i];
    }
    return resultado;
}

//Arma el mensaje "Insumo de <id> listo" que se envia por los pipes
static int construir_mensaje_insumo(char *buffer, const char *id, int limite_max){
    //Se arma el texto
    string msg = "Insumo de " + string(id) + " listo";
    // Copiamos el resultado al buffer de entrada
    size_t i = 0;
    while(i < msg.length() && i < (size_t)(limite_max - 1)){
        buffer[i] = msg[i];
        i++;
    }
    //Se marca el fin de la cadena
    buffer[i] = '\0';
    // Devuelve el largo del mensaje
    return i;
}

//Sube el limite de archivos abiertos (descriptores) para que no faltes pipes
static void subir_limite_fds(){
    //Estructura para almacenar y modificar los límites de recursos del sistema
    struct rlimit limite;
    //Obtiene los límites actuales de archivos abiertos (RLIMIT_NOFILE) del proceso
    getrlimit(RLIMIT_NOFILE, &limite);
    //Si el límite máximo permitido por el sistema operativo es menor a 20000, asigna ese límite, si no, lo fija en 20000
    if(limite.rlim_max < 20000){
        limite.rlim_cur = limite.rlim_max;
    }
    else{
        limite.rlim_cur = 20000;
    }
    //Aplica la configuración de límite de archivos al proceso
    setrlimit(RLIMIT_NOFILE, &limite);
}


//Lectura y construccion del plan

//Procesa el texto del cuarto campo y guarda los IDs de las dependencias
static void parsear_dependencias(string campo, Actividad &act){
    //Limpia los espacios al inicio y al final de la cadena recibida
    string s = trim(campo);
    //Si el texto queda vacio, la actividad no tiene dependencias
    if(s.empty()){
        act.num_deps = 0;
        return;
    }
    //Inicializa el contador de dependencias encontradas
    act.num_deps = 0;
    //Acumulador temporal para construir el ID de cada dependencia
    string actual = "";
    //Recorre el string caracter por caracter
    for(size_t i = 0; i <= s.length(); i++){
        //Si llega al final de la cadena O encuentra una coma, procesa el ID acumulado
        if(i == s.length() || s[i] == ','){
            //Limpia espacios alrededor del ID
            string dep = trim(actual); 
            //Si el ID no esta vacio y aun no superamos el limite maximo de dependencias
            if(!dep.empty() && act.num_deps < max_deps){
                size_t j = 0;
                //Copia caracter por caracter el string "dep" al arreglo de caracteres "dep_ids"
                while(j < dep.length() && j < max_id_len - 1){
                    act.dep_ids[act.num_deps][j] = dep[j];
                    j++;
                }
                //Agrega el caracter nulo para cerrar la cadena
                act.dep_ids[act.num_deps][j] = '\0';
                //Incrementa la cantidad de dependencias
                act.num_deps++;
            }
            //Reinicia el acumulador para leer la siguiente dependencia
            actual = "";
        }
        else{
            //Si es un caracter normal, lo agrega al acumulador actual
            actual += s[i]; 
        }
    }
}

//Lee el archivo linea por linea y llena el arreglo de actividades
static int parsear_plan(const char *ruta_archivo, Actividad actividades[], int *out_n){
    //Abre el archivo en modo lectura
    ifstream archivo(ruta_archivo);
    if(!archivo.is_open()){
        cerr << "Error: No se pudo abrir el archivo " << ruta_archivo << endl;
        return -1;
    }
    string linea_std;
    //Contador de actividades leidas
    int n = 0;
    //Lee el archivo linea por linea hasta el final
    while(getline(archivo, linea_std)){
        string linea = trim(linea_std);
        //Salta las lineas vacias
        if(linea.empty())
          continue;
        //Evita pasar el tamaño maximo del arreglo
        if(n >= max_actividades)
          break;
        //Arreglo para guardar los 4 campos separados por ":"
        string campos[4] = {"", "", "", ""};
        int num_campos = 0;
        //Se separa por ":"
        for(char c : linea){
            if(c == ':'){
                //Al encontrar un ":", avanza al siguiente campo (hasta un maximo de 4 campos)
                if(num_campos < 3){
                    num_campos++;
                }
            }
            else{
                //Va acumulando los caracteres en el campo correspondiente
                campos[num_campos] += c;
            }
        }
        //Referencia a la actividad actual dentro del arreglo
        Actividad &act = actividades[n];
        //Campo 1: copia del id
        string id_str = trim(campos[0]);
        size_t i = 0;
        while(i < id_str.length() && i < max_id_len - 1){
            act.id[i] = id_str[i];
            i++;
        }
        //Cierra la cadena con caracter nulo
        act.id[i] = '\0'; 
        //Campo 2: copia  del nombre
        string nom_str = trim(campos[1]);
        i = 0;
        while(i < nom_str.length() && i < max_nombre_len - 1){
            act.nombre[i] = nom_str[i];
            i++;
        }
        act.nombre[i] = '\0';
        //Campo 3: copia del tiempo de ejecucion en milisegundos
        string tiempo_txt = trim(campos[2]);
        if(tiempo_txt.empty()){
            //Si no tiene tiempo, le da un valor aleatorio dentro del rango
            act.tiempo_ms = tiempo_min_ms + rand() % (tiempo_max_ms - tiempo_min_ms + 1);
        }
        else{
            //Convierte la cadena numerica a entero
            act.tiempo_ms = atoi(tiempo_txt.c_str());
        }
        //Campo 4: extrae las dependencias asociadas
        parsear_dependencias(campos[3], act);
        //Inicializacion de variables de control del proceso
        act.estado = Pendiente;
        act.pid = -1;
        act.num_dependientes = 0;
        //Incrementa el conteo de actividades cargadas
        n++; 
    }
    archivo.close(); 
    //Guarda la cantidad total de actividades leidas a traves del puntero
    *out_n = n;
    return 0;
}

//Busca una actividad por su ID y devuelve su posición en el arreglo
static int buscar_indice_por_id(Actividad actividades[], int n, const char *id){
    //Recorre todas las actividades del arreglo
    for(int i = 0; i < n; i++){    
        //Compara si el ID coincide y retorna el indice
        if(son_iguales(actividades[i].id, id)) 
        return i;  
    }
    return -1;                                             
}

//Modelado del DAG de dependencias

//Conecta las actividades entre si (construye las aristas del grafo DAG)
static int construir_dag(Actividad actividades[], int n){
    //Recorre cada actividad del plan
    for(int i = 0; i < n; i++){    
        //Recorre las dependencias que declara la actividad 'i'
        for(int k = 0; k < actividades[i].num_deps; k++){   
            int idx = buscar_indice_por_id(actividades, n, actividades[i].dep_ids[k]); 
            //Guarda en 'i' la posición del padre que debe esperar
            actividades[i].dep_idx[k] = idx;                                   
            //Obtiene una referencia directa a la actividad padre
            Actividad &padre = actividades[idx];                               
            //Valida no sobrepasar el limite de dependientes
            if(padre.num_dependientes >= max_dependientes){                  
                cout << "Error: '" << padre.id << "' supero el maximo de dependientes" << endl;          
            return -1;                                                     
            }
            //Registra a 'i' como dependiente del padre e incrementa su contador
            padre.dependientes_idx[padre.num_dependientes++] = i;             
        }
    }
    return 0;                                                                 
}
//Crea las pipes para todo el grafo
    for (int i = 0; i < n; i++) {
        Actividad &padre = actividades[i];
        //Recorre la lista de hijos que depende de este padre
        for (int d = 0; d < padre.num_dependientes; d++) {
            int idx_hijo = padre.dependientes_idx[d];
            Actividad &hijo = actividades[idx_hijo];
            //(fds[0] es para lectura, fds[1] es para escritura)
            int fds[2];
            if (pipe(fds) < 0) {
                perror("Error al crear pipe en el DAG");
                return -1;
            }
            //Asigna la escritura al padre
            padre.fd_escritura[d] = fds[1];
            //Asigna la lectura al hijo
            hijo.fd_lectura[hijo.num_fd_lectura] = fds[0];
            hijo.num_fd_lectura++;
        }
    }
    return 0;                                                      
}

//Creación de procesos y scheduler

//Lanza el proceso hijo para ejecutar una actividad y gestionar sus mensajes
static int lanzar_actividad(Actividad actividades[], int idx_actual){
    Actividad &act = actividades[idx_actual];
    //Crea el proceso hijo
    pid_t pid = fork();
    //Falla en la creacion del proceso
    if(pid < 0){
        perror("Error al hacer fork");
        return -1;
    }
    //Hijo
    if(pid == 0){
        //Lee los mensajes que le enviaron sus antecesores
        for(int k = 0; k < act.num_fd_lectura; k++){
            char buffer_mensaje[128];
            //Lee desde el pipe de entrada (fds[0])
            ssize_t bytes_leidos = read(act.fd_lectura[k], buffer_mensaje, sizeof(buffer_mensaje) - 1);
            if(bytes_leidos > 0){
                //Agrega fin de cadena
                buffer_mensaje[bytes_leidos] = '\0';
                cout << "pid " << getpid() << " '" << act.id
                     << "' recibio señal por pipe: " << buffer_mensaje << "" << endl;
            }
            //Cierra el pipe de lectura
            close(act.fd_lectura[k]);
            //Marca como cerrado
            act.fd_lectura[k] = -1; 
        }
        //Muestra mensaje de inicio y simula el tiempo de ejecucion
        cout << "pid " << getpid() << " iniciando actividad " << act.id << " (" << act.nombre << ") " << act.tiempo_ms << " ms" << endl;

        //Convierte milisegundos a microsegundos para usleep
        usleep(act.tiempo_ms * 1000);
        //Evalua si la tarea falla (10% de probabilidad)
        srand(time(NULL) + getpid());
        bool fallo = (rand() % 100) < 10;
        if(fallo){
            cout << "pid " << getpid() << " " << act.id << " fallo" << endl;
            //Si falla, cierra los pipes de salida sin escribir nada
            for(int d = 0; d < act.num_dependientes; d++){
                if(act.fd_escritura[d] >= 0){
                    close(act.fd_escritura[d]);
                    act.fd_escritura[d] = -1;
                }
            }
            //Termina con error
            exit(EXIT_FAILURE); 
        }
        //Si no fallo, envia el mensaje a sus actividades dependientes
        char mensaje[128];
        int largo = construir_mensaje_insumo(mensaje, act.id, 128);
        for(int d = 0; d < act.num_dependientes; d++){
            //Si la tarea dependiente fue abortada, ignora ese pipe
            if (act.fd_escritura[d] < 0)
              continue;
            //Escribe en el pipe de salida y lo cierra
            write(act.fd_escritura[d], mensaje, largo + 1);
            close(act.fd_escritura[d]);
            act.fd_escritura[d] = -1;
        }
        //Termina exitosamente
        exit(EXIT_SUCCESS);
    }
    //Padre
    //El padre cierra sus copias de lectura de esta tarea
    for(int k = 0; k < act.num_fd_lectura; k++){
        if(act.fd_lectura[k] >= 0){
            close(act.fd_lectura[k]);
            act.fd_lectura[k] = -1;
        }
    }
    //Guarda el id del hijo y cambia su estado
    act.pid = pid;
    act.estado = Ejecutando;
    return 0;
}

