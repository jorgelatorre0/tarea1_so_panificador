###Planificador Dieciochero - Tarea 1 Sistemas Operativos

Integrantes: Fernando Letelier - Jorge Latorre

1.- Descripción

Simulador y planificador de actividades. Lee un archivo plan.txt que describe un grafo acíclico dirigido (DAG) de actividades, con su duración y sus dependencias, y las ejecuta cumpliendo dos reglas:

- Una actividad solo comienza cuando todas sus dependencias terminaron bien.
- Nunca hay mas de K procesos corriendo al mismo tiempo.

Cada actividad corre en un proceso hijo (fork) y, al terminar, avisa a las actividades que dependen de ella mediante un mensaje por pipe. Si una actividad falla, solo se aborta la rama que dependía de ella. Con Ctrl+C se simula la llegada de la Seremi y se abortan todas las actividades.

2.- Compilación y uso

Compilar:

    g++ -Wall -Wextra -std=c++17 -o planificador planificador.cpp -lpthread

Ejecutar:

    ./planificador plan.txt K

plan.txt es el archivo con el plan y K es un entero positivo con el máximo de procesos simultáneos. Si faltan argumentos, K no es un entero positivo o el archivo no se puede abrir, el programa muestra un error y termina con código 1.

Formato de plan.txt (una actividad por linea):

    ID : Nombre : tiempo_ms : [Dep1, Dep2, etc]

Si tiempo_ms esta vacío, se asigna un valor aleatorio entre 100 y 5000 ms. Las dependencias van separadas por comas, y pueden estar vacías.

Ejemplo:

    1 : prender_carbon : 500 :
    2 : comprar_carne : 1200 :
    3 : comprar_pan : 300 :
    4 : asar_longaniza : 800 : 1, 2
    5 : armar_choripan : 250 : 3, 4
    6 : servir_mesa : 100 : 5

Con "./planificador plan.txt 2" nunca corren mas de 2 actividades a la vez, y la actividad 4 solo inicia después de recibir los insumos de la 1 y la 2.

Como probar el aislamiento de errores: cada hijo imprime su pid al iniciar. Con un plan de actividades largas, ejecutar "kill -9 "pid"" sobre un hijo desde otra terminal. Esa actividad queda como fallida, las que dependían de ella quedan abortadas y las ramas independientes terminan normalmente.

Para probar la Seremi se debe presionar Ctrl+C durante la ejecución.

3.- Funciones Implementadas

- son_iguales: compara dos textos carácter por carácter.
- trim: quita espacios, tabs y saltos de linea al inicio y al final de un texto.
- construir_mensaje_insumo: arma el mensaje "Insumo de "id" listo" que viaja por los pipes.
- subir_limite_fds: sube el limite de archivos abiertos del proceso.
- parsear_dependencias: convierte el campo de dependencias en una lista de IDs.
- parsear_plan: lee plan.txt, separa los 4 campos por ":", asigna tiempo aleatorio si falta e inicializa cada actividad.
- buscar_indice_por_id: devuelve la posición de una actividad a partir de su ID.
- construir_dag: guarda, para cada actividad, de quien depende y quienes dependen de ella.
- lanzar_actividad: crea los pipes hacia sus dependientes, hace fork y, en el hijo, lee los insumos, simula el trabajo y escribe el mensaje a sus dependientes.
- abortar_rama_recursiva: marca como abortada a una actividad y, recursivamente, a todas las que dependen de ella.
- buscar_por_pid: encuentra que actividad corresponde al pid que devolvió waitpid.
- revisar_dependientes: cuando una actividad termina bien, deja lista a las dependientes que ya tienen todas sus dependencias terminadas.
- ejecutar_plan: scheduler. Lleva una cola de actividades listas, lanza hasta K procesos y espera con waitpid a que termine alguno.
- manejador_seremi: manejador de SIGINT (Ctrl+C). Avisa, envia SIGTERM a los hijos en ejecucion y termina.
- main: valida argumentos, registra SIGINT, sube el limite de descriptores, lee el plan, arma el DAG y lo ejecuta.

Cada actividad tiene un estado (Pendiente, Lista, Ejecutando, Ok, Fallida, Abortada) que el scheduler va actualizando.


