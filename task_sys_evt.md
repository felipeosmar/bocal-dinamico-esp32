● O que é a task "sys_evt"?                                                                                                                                                               
                                                                                                                                                                                          
  "sys_evt" é a abreviação de "System Event" - é a task do Event Loop (loop de eventos) do ESP-IDF.                                                                                       
                                                                                                                                                                                          
  Função                                                                                                                                                                                  
                                                                                                                                                                                          
  Esta task é o coração do sistema de eventos do ESP32. Ela recebe, enfileira e distribui eventos do sistema para os handlers registrados.                                                
                                                                                                                                                                                          
  | Responsabilidade   | Descrição                                      |                                                                                                                 
  |--------------------|------------------------------------------------|                                                                                                                 
  | Receber eventos    | Coleta eventos de WiFi, IP, Ethernet, etc.     |                                                                                                                 
  | Enfileirar eventos | Mantém uma fila de eventos pendentes           |                                                                                                                 
  | Distribuir eventos | Chama os handlers registrados para cada evento |                                                                                                                 
  | Sincronização      | Garante ordem de processamento dos eventos     |                                                                                                                 
                                                                                                                                                                                          
  Exemplos de eventos processados                                                                                                                                                         
                                                                                                                                                                                          
  WIFI_EVENT_STA_START          → WiFi Station iniciou                                                                                                                                    
  WIFI_EVENT_STA_CONNECTED      → Conectou ao Access Point                                                                                                                                
  WIFI_EVENT_STA_DISCONNECTED   → Desconectou do AP                                                                                                                                       
  IP_EVENT_STA_GOT_IP           → Recebeu endereço IP via DHCP                                                                                                                            
  WIFI_EVENT_AP_STACONNECTED    → Cliente conectou ao seu AP                                                                                                                              
  WIFI_EVENT_AP_STADISCONNECTED → Cliente desconectou do seu AP                                                                                                                           
                                                                                                                                                                                          
  Fluxo de eventos                                                                                                                                                                        
                                                                                                                                                                                          
  [Driver WiFi] ──→ [Evento gerado] ──→ [sys_evt processa] ──→ [Seu handler é chamado]                                                                                                    
                                                                                                                                                                                          
  No seu código                                                                                                                                                                           
                                                                                                                                                                                          
  No seu projeto, você provavelmente tem handlers registrados assim:                                                                                                                      
                                                                                                                                                                                          
  // Registro típico de handler de eventos                                                                                                                                                
  ESP_ERROR_CHECK(esp_event_handler_register(                                                                                                                                             
      WIFI_EVENT,           // Tipo de evento                                                                                                                                             
      ESP_EVENT_ANY_ID,     // Qualquer ID                                                                                                                                                
      &wifi_event_handler,  // Sua função handler                                                                                                                                         
      NULL                                                                                                                                                                                
  ));                                                                                                                                                                                     
                                                                                                                                                                                          
  Quando um evento WiFi ocorre, a task sys_evt chama sua função wifi_event_handler.                                                                                                       
                                                                                                                                                                                          
  Por que está "Blocked" com 0% CPU?                                                                                                                                                      
                                                                                                                                                                                          
  | Estado        | Motivo                                  |                                                                                                                             
  |---------------|-----------------------------------------|                                                                                                                             
  | Blocked       | Aguardando eventos na fila (normal)     |                                                                                                                             
  | 0% CPU        | Eventos são raros, processa rapidamente |                                                                                                                             
  | Prioridade 20 | Alta, para responder rapidamente        |                                                                                                                             
  | ~3.2 KB Stack | Suficiente para chamar handlers         |                                                                                                                             
                                                                                                                                                                                          
  A task fica bloqueada esperando eventos. Quando um evento chega na fila, ela acorda, processa, e volta a bloquear.                                                                      
                                                                                                                                                                                          
  Resumo                                                                                                                                                                                  
                                                                                                                                                                                          
  É a task central de eventos do ESP-IDF que permite programação orientada a eventos. Sem ela, você não receberia notificações de conexão WiFi, obtenção de IP, e outros eventos do sistema. É criada automaticamente quando você inicializa o sistema de eventos com esp_event_loop_create_default().