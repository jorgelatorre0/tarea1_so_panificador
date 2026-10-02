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
        //Campo 1: copia del ID
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
        //Campo 3: tiempo de ejecucion en milisegundos
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

