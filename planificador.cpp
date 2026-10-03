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

//Modelado del DAG de dependencias

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
                cout << "Error: '" << padre.id
                     << "' supero el maximo de dependientes" << endl;          
            return -1;                                                     
            }
            //Registra a 'i' como dependiente del padre e incrementa su contador
            padre.dependientes_idx[padre.num_dependientes++] = i;             
        }
    }
    return 0;                                                                 
}

