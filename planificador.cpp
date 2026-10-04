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

//Largo máximo del ID de una actividad
#define max_id_len 64
//Largo máximo del nombre de una actividad
#define max_nombre_len 128
//Máximo de dependencias que puede tener una actividad
#define max_deps 20
// Máximo de actividades que pueden depender de una sola
#define max_dependientes 1000
//Máximo de actividades del plan (10000+1 de margen)
#define max_actividades 10001
//Tiempo mínimo al asignar duración aleatoria
#define tiempo_min_ms 100
//Tiempo máximo al asignar duración aleatoria
#define tiempo_max_ms 5000

//Estados por lo que pasa una actividad
enum EstadoActividad {Pendiente, Lista, Ejecutando, Ok, Fallida, Abortada};

//Información de una actividad del plan
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

//Arma el mensaje "Insumo de <id> listo" que se envía por los pipes
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

//Lectura y construcción del plan

//Procesa el texto del cuarto campo y guarda los IDs de las dependencias
static void parsear_dependencias(string campo, Actividad &act){
    //Limpia los espacios al inicio y al final de la cadena recibida
    string s = trim(campo);
    //Si el texto viene entre corchetes, como en "[1, 2]", los quita
    if(s.length() >= 2 && s[0] == '[' && s[s.length() - 1] == ']'){
        s = trim(s.substr(1, s.length() - 2));
    }
    //Inicializa el contador de dependencias encontradas
    act.num_deps = 0;
    //Si el texto queda vacío, la actividad no tiene dependencias
    if(s.empty()){
        return;
    }
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
    //Semilla para los tiempos aleatorios (sin esto salen siempre iguales)
    srand(time(NULL));
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
        //Campo 2: copia del nombre
        string nom_str = trim(campos[1]);
        i = 0;
        while(i < nom_str.length() && i < max_nombre_len - 1){
            act.nombre[i] = nom_str[i];
            i++;
        }
        act.nombre[i] = '\0';
        //Campo 3: copia del tiempo de ejecución en milisegundos
        string tiempo_txt = trim(campos[2]);
        if(tiempo_txt.empty()){
            //Si no tiene tiempo, le da un valor aleatorio dentro del rango
            act.tiempo_ms = tiempo_min_ms + rand() % (tiempo_max_ms - tiempo_min_ms + 1);
        }
        else{
            //Convierte la cadena numérica a entero
            act.tiempo_ms = atoi(tiempo_txt.c_str());
        }
        //Campo 4: extrae las dependencias asociadas
        parsear_dependencias(campos[3], act);
        //Inicialización de variables de control del proceso
        act.estado = Pendiente;
        act.pid = -1;
        act.num_dependientes = 0;
        act.num_fd_lectura = 0;
        //Incrementa el conteo de actividades cargadas
        n++; 
    }
    archivo.close(); 
    //Guarda la cantidad total de actividades leídas a través del puntero
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
//Creación de procesos y paso de mensajes por pipes

//Lanza el proceso hijo para ejecutar una actividad y gestionar sus mensajes
static int lanzar_actividad(Actividad actividades[], int idx_actual){
    Actividad &act = actividades[idx_actual];
    //Crea un pipe por cada actividad que depende de esta (se crean justo antes del fork y no todos de una vez, para no agotar los descriptores de archivo)
    for(int d = 0; d < act.num_dependientes; d++){
        Actividad &hijo = actividades[act.dependientes_idx[d]];
        //Si el dependiente ya fue abortado, no hace falta crear el pipe
        if(hijo.estado == Abortada){
            act.fd_escritura[d] = -1;
            continue;
        }
        //[0] es para lectura, [1] es para escritura
        int fds[2];
        if(pipe(fds) < 0){
            perror("Error al crear pipe");
            return -1;
        }
        //Esta actividad escribirá por fds[1]
        act.fd_escritura[d] = fds[1];
        //El dependiente leerá por fds[0]
        hijo.fd_lectura[hijo.num_fd_lectura] = fds[0];
        hijo.num_fd_lectura++;
    }
    //Crea el proceso hijo
    pid_t pid = fork();
    //Falla en la creación del proceso
    if(pid < 0){
        perror("Error al hacer fork");
        return -1;
    }
    //Hijo
    if(pid == 0){
        //El hijo no usa el manejador de la Seremi, así Ctrl+C lo termina sin imprimir el mensaje de nuevo
        signal(SIGINT, SIG_DFL);
        //Lee los mensajes que le enviaron sus antecesores
        for(int k = 0; k < act.num_fd_lectura; k++){
            char buffer_mensaje[128];
            //Lee desde el pipe de entrada fds[0]
            ssize_t bytes_leidos = read(act.fd_lectura[k], buffer_mensaje, sizeof(buffer_mensaje) - 1);
            if(bytes_leidos > 0){
                //Agrega fin de cadena
                buffer_mensaje[bytes_leidos] = '\0';
                cout << "pid " << getpid() << " '" << act.id
                     << "' recibio insumo por pipe: " << buffer_mensaje << endl;
            }
            //Cierra el pipe de lectura
            close(act.fd_lectura[k]);
        }
        //Muestra mensaje de inicio y simula el tiempo de ejecucion
        cout << "pid " << getpid() << " iniciando actividad " << act.id << " (" << act.nombre << ") " << act.tiempo_ms << " ms" << endl;
        //Convierte milisegundos a microsegundos para usleep
        usleep(act.tiempo_ms * 1000);
        //Envía el mensaje a sus actividades dependientes
        char mensaje[128];
        int largo = construir_mensaje_insumo(mensaje, act.id, 128);
        for(int d = 0; d < act.num_dependientes; d++){
            //Si la tarea dependiente fue abortada, ignora ese pipe
            if(act.fd_escritura[d] < 0)
              continue;
            //Escribe en el pipe de salida (si falla la escritura, la actividad termina con error)
            if(write(act.fd_escritura[d], mensaje, largo + 1) < 0)
              exit(EXIT_FAILURE);
            //Cierra el pipe de salida
            close(act.fd_escritura[d]);
        }
        //Termina exitosamente
        exit(EXIT_SUCCESS);
    }
    //Padre
    //El padre cierra sus copias de lectura de esta tarea (las usa el hijo)
    for(int k = 0; k < act.num_fd_lectura; k++){
        close(act.fd_lectura[k]);
    }
    //El padre cierra sus copias de escritura (las usa el hijo)
    for(int d = 0; d < act.num_dependientes; d++){
        if(act.fd_escritura[d] >= 0){
            close(act.fd_escritura[d]);
        }
    }
    //Guarda el id del hijo y cambia su estado
    act.pid = pid;
    act.estado = Ejecutando;
    return 0;
}

//Abortar ramas para aislamiento de errores
static void abortar_rama_recursiva(Actividad actividades[], int idx, int &terminadas){
    //Obtiene referencia a la actividad actual en el arreglo con su indice
    Actividad &act = actividades[idx];
    //Si la tarea ya fue abortada o fallo, no la procesa de nuevo
    if(act.estado == Abortada || act.estado == Fallida)
      return;
    //Cambia el estado porque su antecedente fallo
    act.estado = Abortada;
    terminadas++;
    //Muestra la cancelación
    cout << "Actividad '" << act.id << "' abortada por falla en su rama dependiente" << endl;
    //Recorre las tareas hijas que dependían de esta actividad y las aborta
    for(int i = 0; i < act.num_dependientes; i++){
        abortar_rama_recursiva(actividades, act.dependientes_idx[i], terminadas);
    }
}

//Busca la actividad asociada a un pid que esta corriendo
static int buscar_por_pid(Actividad actividades[], int n, pid_t pid){
    for(int i = 0; i < n; i++){
        //Verifica que la actividad este en ejecución y que el pid coincida con el retornado por waitpid
        if(actividades[i].estado == Ejecutando && actividades[i].pid == pid)
          //Retorna el indice de la actividad encontrada en el arreglo
          return i;
    }
    return -1;
}
