● O que é a task "Tmr Svc"?                                                                                                                                                               
                                                                                                                                                                                          
  "Tmr Svc" é a abreviação de "Timer Service" - é a task do FreeRTOS Software Timer que gerencia todos os timers por software do sistema.                                                 
                                                                                                                                                                                          
  Função                                                                                                                                                                                  
                                                                                                                                                                                          
  Esta task é responsável por executar os callbacks dos software timers do FreeRTOS quando eles expiram.                                                                                  
                                                                                                                                                                                          
  | Responsabilidade        | Descrição                                            |                                                                                                      
  |-------------------------|------------------------------------------------------|                                                                                                      
  | Gerenciar timers        | Controla todos os software timers criados            |                                                                                                      
  | Executar callbacks      | Chama as funções quando os timers expiram            |                                                                                                      
  | Manter fila de comandos | Recebe comandos de start/stop/reset de timers        |                                                                                                      
  | Ordenar execução        | Garante que timers sejam executados na ordem correta |                                                                                                      
                                                                                                                                                                                          
  Software Timers vs Hardware Timers                                                                                                                                                      
                                                                                                                                                                                          
  | Tipo           | Característica                                                    |                                                                                                  
  |----------------|-------------------------------------------------------------------|                                                                                                  
  | Hardware Timer | Usa periférico de hardware, muito preciso, limitado em quantidade |                                                                                                  
  | Software Timer | Gerenciado pelo FreeRTOS, menos preciso, quantidade ilimitada     |                                                                                                  
                                                                                                                                                                                          
  A task Tmr Svc gerencia apenas os software timers.                                                                                                                                      
                                                                                                                                                                                          
  Como funciona                                                                                                                                                                           
                                                                                                                                                                                          
  [Sua task] ─── xTimerStart() ───→ [Fila de comandos] ───→ [Tmr Svc processa]                                                                                                            
                                                                    │                                                                                                                     
  [Callback executado] ←─── timer expirou ←─────────────────────────┘                                                                                                                     
                                                                                                                                                                                          
  Exemplo de uso                                                                                                                                                                          
                                                                                                                                                                                          
  // Criar um timer que dispara a cada 1 segundo                                                                                                                                          
  TimerHandle_t timer = xTimerCreate(                                                                                                                                                     
      "MeuTimer",           // Nome                                                                                                                                                       
      pdMS_TO_TICKS(1000),  // Período: 1000ms                                                                                                                                            
      pdTRUE,               // Auto-reload: sim                                                                                                                                           
      NULL,                 // ID                                                                                                                                                         
      timer_callback        // Função callback                                                                                                                                            
  );                                                                                                                                                                                      
                                                                                                                                                                                          
  // Iniciar o timer                                                                                                                                                                      
  xTimerStart(timer, 0);                                                                                                                                                                  
                                                                                                                                                                                          
  // Quando expira, Tmr Svc chama:                                                                                                                                                        
  void timer_callback(TimerHandle_t xTimer) {                                                                                                                                             
      // Seu código aqui                                                                                                                                                                  
  }                                                                                                                                                                                       
                                                                                                                                                                                          
  Por que está "Blocked" com 0% CPU?                                                                                                                                                      
                                                                                                                                                                                          
  | Estado        | Motivo                                              |                                                                                                                 
  |---------------|-----------------------------------------------------|                                                                                                                 
  | Blocked       | Aguardando próximo timer expirar ou comando na fila |                                                                                                                 
  | 0% CPU        | Callbacks são rápidos, executa e volta a dormir     |                                                                                                                 
  | Prioridade 1  | Baixa (configurável via configTIMER_TASK_PRIORITY)  |                                                                                                                 
  | ~6.1 KB Stack | Para executar callbacks dos timers                  |                                                                                                                 
                                                                                                                                                                                          
  Importante                                                                                                                                                                              
                                                                                                                                                                                          
  Os callbacks dos software timers não devem bloquear ou demorar muito, pois:                                                                                                             
  1. Executam no contexto da task Tmr Svc                                                                                                                                                 
  2. Bloqueiam outros timers de serem processados                                                                                                                                         
  3. Podem causar atrasos em todo o sistema de timers                                                                                                                                     
                                                                                                                                                                                          
  Resumo                                                                                                                                                                                  
                                                                                                                                                                                          
  É a task daemon do FreeRTOS que gerencia software timers. Criada automaticamente pelo FreeRTOS quando configUSE_TIMERS está habilitado. Permite criar timers periódicos ou one-shot sem precisar criar tasks dedicadas para cada um.