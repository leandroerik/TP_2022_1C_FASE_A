#include <proceso.h>
#include <kernel_utils.h>

/*Inicializadores*/
void inicializar_semaforos()
{
    pthread_mutex_init(&mutexNumeroProceso, NULL);
    pthread_mutex_init(&mutexColaNuevos, NULL);
    pthread_mutex_init(&mutexColaListos, NULL);
    pthread_mutex_init(&mutexColaEjecutando, NULL);
    pthread_mutex_init(&mutexColaBloqueados, NULL);

    pthread_mutex_init(&mutexColaSuspendidoListo, NULL);
    pthread_mutex_init(&mutexColaFinalizado, NULL);

    sem_init(&semaforoProcesoNuevo, 0, 0);
    sem_init(&semaforoProcesoListo, 0, 0);

    sem_init(&contadorBloqueados, 0, 0);

    sem_init(&analizarSuspension, 0, 0);
    sem_init(&suspensionFinalizada, 0, 0);

    sem_init(&semaforoCantidadProcesosEjecutando, 0, 1);

    sem_init(&despertarPlanificadorLargoPlazo, 0, 0);
}
void inicializar_colas_procesos()
{
    colaNuevos = queue_create();
    colaListos = list_create();
    colaEjecutando = queue_create();
    colaBloqueados = queue_create();
    colaSuspendidoListo = queue_create();
    colaFinalizado = queue_create();
}

void iniciar_planificadores()
{
    pthread_create(&hilo_planificador_largo_plazo, NULL, planificador_largo_plazo, NULL);

    if (strcmp(KERNEL_CONFIG.ALGORITMO_PLANIFICACION, "FIFO") != 0)
    {
        pthread_create(&hilo_planificador_corto_plazo, NULL, planificador_corto_plazo_srt, NULL);
    }
    else
    {
        pthread_create(&hilo_planificador_corto_plazo, NULL, planificador_corto_plazo_fifo, NULL);
    }

    pthread_create(&hilo_dispositivo_io, NULL, dispositivo_io, NULL);
}

/*Funciones del proceso*/

void ejecutar(Pcb *proceso)
{
    proceso->escenario->estado = EJECUTANDO;

    int socketDispatch = conectar_con_cpu_dispatch();

    enviar_pcb(proceso, socketDispatch);

    log_info(logger, "Envio el proceso con PID : %d de CPU!", proceso->pid);

    CodigoOperacion codOperacion = obtener_codigo_operacion(socketDispatch);

    Pcb *procesoRecibido;

    switch (codOperacion)
    {
    case PCB:
        procesoRecibido = deserializar_pcb(socketDispatch);
        log_info(logger, "Recibi el proceso con PID : %d de CPU!", proceso->pid);
        manejar_proceso_recibido(procesoRecibido, socketDispatch);

        break;

    case DESCONEXION:
        log_info(logger, "Se desconectó el CPU Dispatch. %d", codOperacion);
        break;

    default:
        log_warning(logger, "Operación desconocida.");
        break;
    }

    liberar_conexion_con_servidor(socketDispatch);
}

void manejar_proceso_recibido(Pcb *pcb, int socketDispatch)
{
    sacar_proceso_ejecutando();
    // Pongo la rafaga anterior real
    pcb->tiempoRafagaRealAnterior = obtener_tiempo_actual() - pcb->tiempoInicioEjecucion;

    switch (pcb->escenario->estado)
    {
    case INTERRUMPIDO:
        log_info(logger, "[INTERRUPCION]Proceso : [%d] fue INTERRUPIDO.", pcb->pid);
        // Libero la conexion asi no se bloquea el cpu en la espera de un codigo de operacion(Asi puede espera a otro procesos)
        liberar_conexion_con_servidor(socketDispatch);
        log_info(logger, "tiempo ejecucion :%d", obtener_tiempo_actual() - pcb->tiempoInicioEjecucion);
        manejar_proceso_interrumpido(pcb);

        break;

    case BLOQUEADO_IO:
        log_info(logger, "Proceso: [%d] (%d seg.)se movio a BLOQUEADO", pcb->pid, pcb->escenario->tiempoBloqueadoIO / 1000);
        agregar_proceso_bloqueado(pcb);

        /*crear hilo que cuente hasta 10 y suspenda al proceso si sigue bloqueado*/
        Hilo hiloMonitorizacionSuspension;

        pthread_create(&hiloMonitorizacionSuspension, NULL, (void *)monitorizarSuspension, pcb);
        break;

    case TERMINADO:
        agregar_proceso_finalizado(pcb);

        decrementar_cantidad_procesos_memoria();
        log_info(logger, "Procesos en MEMORIA: %d", cantidadProcesosEnMemoria);
        break;

    default:
        log_info(logger, "El proceso %d es medio raro", pcb->pid);
        break;
    }
}

void manejar_proceso_interrumpido(Pcb *pcb)
{
    // Inicialmente es el pcb ,a menos que sea reemplazado por el mas corto
    Pcb *pcbEjecutar = pcb;
    // comparar lo que le falta con las estimaciones de los listos

    float tiempoQueYaEjecuto = obtener_tiempo_actual() - pcb->tiempoInicioEjecucion;

    float estimacionEnSegundos = pcb->estimacionRafaga / 1000;

    float tiempoRestanteEnSegundos = estimacionEnSegundos - tiempoQueYaEjecuto;

    log_info(logger, "[INTERRUPCION] Proceso :[%d] ,(cpu:%f),est restante: %f.", pcb->pid, tiempoQueYaEjecuto, tiempoRestanteEnSegundos);
    // Reordenando la cola

    list_sort(colaListos, &ordenar_segun_tiempo_de_trabajo);

    log_info(logger, "[INTERRUPCION] Se reordenara la cola.");

    if (!list_is_empty(colaListos))
    {
        log_info(logger, "[INTERRUPCION]Analizando a cual seleccionar para ejecutar.");
        Pcb *pcbMasCortoDeListos = list_get(colaListos, 0);
        int tiempoPcbMasCortoEnSegundos = obtener_tiempo_de_trabajo(pcbMasCortoDeListos) / 1000;
        log_info(logger, "[INTERRUPCION] El menor tiene [%d] ,est restante: %d.", pcbMasCortoDeListos->pid, tiempoPcbMasCortoEnSegundos);

        if (tiempoRestanteEnSegundos > tiempoPcbMasCortoEnSegundos)
        {
            log_info(logger, "Proceso:[%d] se bloquea por tener tiempo restante largo.", pcbEjecutar->pid);
            agregar_proceso_listo(pcb);
            // aca pongo a ejecutar al mas corto,caso contrario sigue ejecutando el otro
            sem_wait(&semaforoProcesoListo); // Asi decremento el semaforo
            pcbEjecutar = sacar_proceso_mas_corto(colaListos);
        }
    }
    log_info(logger, "[INTERRUPCION] Proceso:[%d] se vuelve a ejecutar (puntero en %d)", pcbEjecutar->pid, pcbEjecutar->contadorPrograma);

    agregar_proceso_ejecutando(pcbEjecutar);
    ejecutar(pcbEjecutar);
}

void *monitorizarSuspension(Pcb *proceso)
{
    int tiempoMaximoBloqueo = KERNEL_CONFIG.TIEMPO_MAXIMO_BLOQUEADO;
    /*Si pasados los segundos establecidos en el config seguis bloqueado ,entonces supendete*/
    int tiempoMaximoBloqueoEnMicrosegundos = tiempoMaximoBloqueo * 1000;
    usleep(tiempoMaximoBloqueoEnMicrosegundos);

    if (proceso->escenario->estado == BLOQUEADO_IO)
    {
        proceso->escenario->estado = SUSPENDIDO;
        log_info(logger, "El proceso: [%d](%d seg) ,se movio a SUSPENDIDO-BLOQUEADO", proceso->pid, tiempo_total_bloqueado(proceso));
        decrementar_cantidad_procesos_memoria();
        /*TODO: Avisar a memoria*/
    }
}

/*Varios*/

void enviar_pcb(Pcb *proceso, int socketDispatch)
{

    Paquete *paquete = crear_paquete(PCB);

    serializar_pcb(paquete, proceso);

    enviar_paquete_a_servidor(paquete, socketDispatch);

    eliminar_paquete(paquete);
}

void *queue_peek_at(t_queue *self, int index)
{
    return list_get(self->elements, index);
}

char *leer_cola(t_queue *cola)
{
    char *out = string_new();

    for (int i = 0; i < queue_size(cola); i++)
    {

        Pcb *proceso_actual = queue_peek_at(cola, i);

        string_append(&out, "[");

        string_append(&out, string_itoa(proceso_actual->pid));
        string_append(&out, "]");
    }
    return out;
}

/*Planificadores*/

void *dispositivo_io()
{
    while (1)
    {

        sem_wait(&contadorBloqueados);

        Pcb *proceso = queue_peek(colaBloqueados);

        int tiempoBloqueo = proceso->escenario->tiempoBloqueadoIO;

        log_info(logger, "[DISP I/O] Proceso: [%d] ,se bloqueara %d segundos", proceso->pid, tiempoBloqueo / 1000);
        /*bloqueo el proceso*/
        int tiempoBloqueoEnMicrosegundos = tiempoBloqueo * 1000;

        usleep(tiempoBloqueoEnMicrosegundos);

        /* Aca lo saco de la cola de bloqueados.*/
        proceso = sacar_proceso_bloqueado();

        if (proceso->escenario->estado == SUSPENDIDO)
        {
            // Agregar a suspendido listo
            agregar_proceso_suspendido_listo(proceso);
        }
        else
        {
            // Sino pasarlo listo.
            agregar_proceso_listo(proceso);
        }
    }
}

void *planificador_largo_plazo()
{
    log_info(logger, "Inicio planificacion LARGO PLAZO [%s]", KERNEL_CONFIG.ALGORITMO_PLANIFICACION);
    while (1)
    {
        sem_wait(&despertarPlanificadorLargoPlazo);
        log_info(logger, "[LARGO-PLAZO] Procesos en MEMORIA: %d", cantidadProcesosEnMemoria);

        if (cantidadProcesosEnMemoria < KERNEL_CONFIG.GRADO_MULTIPROGRAMACION && (queue_size(colaNuevos) > 0 || queue_size(colaSuspendidoListo) > 0))
        {
            Pcb *procesoSaliente;

            procesoSaliente = queue_is_empty(colaSuspendidoListo) ? extraer_proceso_nuevo() : extraer_proceso_suspendido_listo();

            procesoSaliente->escenario->estado = LISTO;

            agregar_proceso_listo(procesoSaliente);
            // Envio interrupcion por cada vez que que entra uno a ready
            bool esSrt = strcmp(KERNEL_CONFIG.ALGORITMO_PLANIFICACION, "SRT") == 0;

            if (esSrt)
            {
                enviar_interrupcion();
            }

            incrementar_cantidad_procesos_memoria();
        }
    }
}

void imprimir_colas()
{
    log_info(logger, "\
    \n\tCola nuevos: %s \
    \n\tCola listos: %s \
    \n\tCola ejecutando: %s \
    \n\tCola bloqueados: %s\
    \n\tCola suspended - ready: % s\
    \n\tCola terminados: %s",
             leer_cola(colaNuevos),
             leer_lista(colaListos), leer_cola(colaEjecutando), leer_cola(colaBloqueados), leer_cola(colaSuspendidoListo), leer_cola(colaFinalizado));
}
char *leer_lista(t_list *cola)
{
    char *out = string_new();

    for (int i = 0; i < list_size(cola); i++)
    {

        Pcb *proceso_actual = list_get(cola, i);

        string_append(&out, "[");

        string_append(&out, string_itoa(proceso_actual->pid));
        string_append(&out, "]");
    }
    return out;
}
void *planificador_corto_plazo_fifo()
{
    log_info(logger, "INICIO PLANIFICACION FIFO");
    while (1)
    {
        sem_wait(&semaforoProcesoListo);
        sem_wait(&semaforoCantidadProcesosEjecutando);

        Pcb *procesoEjecutar = sacar_proceso_listo();

        agregar_proceso_ejecutando(procesoEjecutar);

        ejecutar(procesoEjecutar);
    }
}

void *planificador_corto_plazo_srt()
{
    log_info(logger, "INICIO PLANIFICACION SRT");

    while (1)
    {

        sem_wait(&semaforoProcesoListo);
        sem_wait(&semaforoCantidadProcesosEjecutando);
        log_info(logger, "[PLANI CORTO PLAZO]");

        Pcb *procesoEjecutar = sacar_proceso_mas_corto();

        agregar_proceso_ejecutando(procesoEjecutar);

        ejecutar(procesoEjecutar);
    }
}
Pcb *sacar_proceso_mas_corto()
{

    Pcb *pcbSaliente;
    /*Replanifico la cola de listos*/

    pthread_mutex_lock(&mutexColaListos);

    log_info(logger, "Ordenando proceso mas cortos");

    list_sort(colaListos, &ordenar_segun_tiempo_de_trabajo);

    pcbSaliente = list_remove(colaListos, 0);

    log_info(logger, "\nPID :[%d] ESTIMACION: %f,RAFAGA ANTERIOR: %d -> RESULTADO: %d \n", pcbSaliente->pid, pcbSaliente->estimacionRafaga, pcbSaliente->tiempoRafagaRealAnterior / 1000, obtener_tiempo_de_trabajo(pcbSaliente) / 1000);

    pthread_mutex_unlock(&mutexColaListos);

    return pcbSaliente;
}

float obtener_tiempo_de_trabajo(Pcb *proceso)
{
    float alfa = KERNEL_CONFIG.ALFA;
    float estimacionAnterior = proceso->estimacionRafaga;
    int rafagaAnterior = proceso->tiempoRafagaRealAnterior * 1000;
    float resultado = alfa * rafagaAnterior + (1 - alfa) * estimacionAnterior;

    return resultado;
}

/*Funciones para aniadir proceso a cola*/

void agregar_proceso_nuevo(Pcb *procesoNuevo)
{
    pthread_mutex_lock(&mutexColaNuevos);
    queue_push(colaNuevos, procesoNuevo);
    log_info(logger, "Proceso:[%d] se movio NUEVO.", procesoNuevo->pid);
    pthread_mutex_unlock(&mutexColaNuevos);

    /*Despierto al Planificador de Largo Plazo*/
    sem_post(&despertarPlanificadorLargoPlazo);

    imprimir_colas();
}
void agregar_proceso_listo(Pcb *procesoListo)
{
    pthread_mutex_lock(&mutexColaListos);
    procesoListo->escenario->estado = LISTO;
    list_add(colaListos, procesoListo);
    log_info(logger, "Proceso:[%d] se movio LISTO.", procesoListo->pid);

    pthread_mutex_unlock(&mutexColaListos);

    sem_post(&semaforoProcesoListo);

    imprimir_colas();
}

void agregar_proceso_ejecutando(Pcb *procesoEjecutando)
{
    pthread_mutex_lock(&mutexColaEjecutando);
    if (procesoEjecutando->escenario->estado != INTERRUMPIDO)
    {
        // Aca si pongo tiempo inicio ejecucion asi no cambia por cada interrupcion
        procesoEjecutando->tiempoInicioEjecucion = obtener_tiempo_actual();
        log_info(logger, "Se puso tiempo inicio ejecucion proceso %d", procesoEjecutando->pid);
    }

    queue_push(colaEjecutando, procesoEjecutando);
    log_info(logger, "Proceso:[%d] se movio EJECUTANDO.", procesoEjecutando->pid);

    pthread_mutex_unlock(&mutexColaEjecutando);

    imprimir_colas();
}

void agregar_proceso_bloqueado(Pcb *procesoBloqueado)
{

    pthread_mutex_lock(&mutexColaBloqueados);
    // Se guarda la estimacion anterior
    procesoBloqueado->estimacionRafaga = obtener_tiempo_de_trabajo(procesoBloqueado);
    /*Agrego tiempo incial BLOQUEO*/
    procesoBloqueado->tiempoInicioBloqueo = obtener_tiempo_actual();
    // Se guarda tiempo de ejecucion
    procesoBloqueado->tiempoRafagaRealAnterior = obtener_tiempo_actual() - procesoBloqueado->tiempoInicioEjecucion;

    log_info(logger, "Se actualiza la rafaga real anterior a :%d", procesoBloqueado->tiempoRafagaRealAnterior);
    log_info(logger, "Se actualiza la rafaga estimada anterior a :%d", procesoBloqueado->estimacionRafaga);

    procesoBloqueado->escenario->estado = BLOQUEADO_IO;

    queue_push(colaBloqueados, procesoBloqueado);
    log_info(logger, "Proceso: [%d] se movio a BLOQUEADO", procesoBloqueado->pid);

    pthread_mutex_unlock(&mutexColaBloqueados);

    /*Aviso al dispositivo de E/S*/
    sem_post(&contadorBloqueados);
    // Despierto al plani de largo plazo
    // (asi al proceso bloqueado lo encola para luego ser ejecutado)
    sem_post(&despertarPlanificadorLargoPlazo);

    imprimir_colas();
}
void agregar_proceso_finalizado(Pcb *procesoFinalizado)
{
    pthread_mutex_lock(&mutexColaFinalizado);

    queue_push(colaFinalizado, procesoFinalizado);
    log_info(logger, "Proceso:[%d] se encuentra TERMINADO.", procesoFinalizado->pid);

    pthread_mutex_unlock(&mutexColaFinalizado);

    /*Despierto al Planificador de Largo Plazo*/
    sem_post(&despertarPlanificadorLargoPlazo);

    imprimir_colas();
}

void agregar_proceso_suspendido_listo(Pcb *procesoSuspendidoListo)
{
    pthread_mutex_lock(&mutexColaSuspendidoListo);
    procesoSuspendidoListo->escenario->estado = LISTO;
    queue_push(colaSuspendidoListo, procesoSuspendidoListo);
    log_info(logger, "Proceso:[%d] se encuentra SUSPENDIDO-LISTO.", procesoSuspendidoListo->pid);

    pthread_mutex_unlock(&mutexColaSuspendidoListo);

    /*Despierto al Planificador de Largo Plazo*/
    sem_post(&despertarPlanificadorLargoPlazo);

    imprimir_colas();
}

/*Funciones para sacar procesos a cola.*/

void sacar_proceso_ejecutando()
{

    pthread_mutex_lock(&mutexColaEjecutando);
    Pcb *pcbSaliente = queue_pop(colaEjecutando);
    log_info(logger, "Proceso : [%d] salío de EJECUTANDO.", pcbSaliente->pid);
    pthread_mutex_unlock(&mutexColaEjecutando);

    sem_post(&semaforoCantidadProcesosEjecutando);

    /*Despierto al Planificador de Largo Plazo*/
    sem_post(&despertarPlanificadorLargoPlazo);

    imprimir_colas();
}

Pcb *sacar_proceso_bloqueado()
{
    Pcb *pcbSaliente;

    pthread_mutex_lock(&mutexColaBloqueados);
    pcbSaliente = queue_pop(colaBloqueados);
    log_info(logger, "Proceso : [%d] salío de BLOQUEADO. (real ant : %d)", pcbSaliente->pid, pcbSaliente->tiempoRafagaRealAnterior);
    pthread_mutex_unlock(&mutexColaBloqueados);
    // Envio interrupcion por cada vez quesale de bloqueado
    bool esSrt = strcmp(KERNEL_CONFIG.ALGORITMO_PLANIFICACION, "SRT") == 0;

    if (esSrt)
    {
        enviar_interrupcion();
    }

    return pcbSaliente;
}

Pcb *sacar_proceso_listo()
{

    pthread_mutex_lock(&mutexColaListos);
    Pcb *pcbSaliente = list_remove(colaListos, 0);
    log_info(logger, "Proceso : [%d] salío de LISTO.", pcbSaliente->pid);
    pthread_mutex_unlock(&mutexColaListos);

    return pcbSaliente;
}

Pcb *extraer_proceso_nuevo()
{
    pthread_mutex_lock(&mutexColaNuevos);

    Pcb *pcbSaliente = queue_pop(colaNuevos);
    log_info(logger, "Proceso : [%d] salío de NUEVO.", pcbSaliente->pid);

    pthread_mutex_unlock(&mutexColaNuevos);

    return pcbSaliente;
}

Pcb *extraer_proceso_suspendido_listo()
{
    pthread_mutex_lock(&mutexColaSuspendidoListo);

    Pcb *pcbSaliente = queue_pop(colaSuspendidoListo);
    log_info(logger, "Proceso : [%d] salío de SUSPENDIDO-LISTO.", pcbSaliente->pid);

    pthread_mutex_unlock(&mutexColaSuspendidoListo);

    return pcbSaliente;
}

/*Monitores*/
void incrementar_cantidad_procesos_memoria()
{
    pthread_mutex_lock(&mutexcantidadProcesosMemoria);
    cantidadProcesosEnMemoria++;
    pthread_mutex_unlock(&mutexcantidadProcesosMemoria);
}
void decrementar_cantidad_procesos_memoria()
{
    pthread_mutex_lock(&mutexcantidadProcesosMemoria);
    cantidadProcesosEnMemoria--;
    pthread_mutex_unlock(&mutexcantidadProcesosMemoria);
}

int obtener_tiempo_actual()
{
    return time(NULL);
}

bool ordenar_segun_tiempo_de_trabajo(void *procesoA, void *procesoB)
{
    return obtener_tiempo_de_trabajo((Pcb *)procesoA) < obtener_tiempo_de_trabajo((Pcb *)procesoB);
}

int tiempo_total_bloqueado(Pcb *proceso)
{
    return obtener_tiempo_actual() - proceso->tiempoInicioBloqueo;
}