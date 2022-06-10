#include <kernel_utils.h>

int main(void)
{
    idProcesoGlobal = 0;
    cantidadProcesosEnMemoria = 0;

    Config *config = config_create("Kernel.config");
    logger = iniciar_logger_kernel();

    rellenar_configuracion_kernel(config);

    log_info(logger, "Iniciando Servidor Kernel...");
    int socketKernel = iniciar_servidor_kernel();

    if (socketKernel < 0)
    {
        log_error(logger, "Error intentando iniciar Servidor Kernel.");
        return EXIT_FAILURE;
    }

    log_info(logger, "Servidor Kernel iniciado correctamente.");

    inicializar_semaforos();
    inicializar_colas_procesos();
    iniciar_planificadores();

    Hilo hiloConsolas;
    Hilo hiloConexionMemoria;

    pthread_create(&hiloConsolas, NULL, (void *)esperar_consola, (void *)socketKernel);
    pthread_create(&hiloConexionMemoria, NULL, (void *)manejar_conexion_memoria, NULL);

    pthread_join(hiloConsolas, NULL);
    pthread_join(hiloConexionMemoria, NULL);

    apagar_servidor(socketKernel);
    log_info(logger, "Servidor Kernel finalizado.");

    log_destroy(logger);
    config_destroy(config);

    return EXIT_SUCCESS;
}