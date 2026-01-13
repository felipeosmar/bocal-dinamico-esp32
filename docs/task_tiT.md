● O que é a task "tiT"?                                                                                                                                                                   
                                                                                                                                                                                          
  "tiT" é a abreviação de "TCP/IP Thread" - é a task principal do LwIP (Lightweight IP), a pilha de rede usada pelo ESP32.                                                                
                                                                                                                                                                                          
  Função                                                                                                                                                                                  
                                                                                                                                                                                          
  Esta task é responsável por:                                                                                                                                                            
                                                                                                                                                                                          
  | Responsabilidade         | Descrição                                             |                                                                                                    
  |--------------------------|-------------------------------------------------------|                                                                                                    
  | Processamento de pacotes | Recebe e processa pacotes de rede vindos da task wifi |                                                                                                    
  | Pilha TCP/IP             | Gerencia conexões TCP, UDP, DNS, DHCP, etc.           |                                                                                                    
  | Callbacks de rede        | Executa callbacks de sockets e conexões               |                                                                                                    
  | Buffers de rede          | Gerencia os buffers de transmissão/recepção           |                                                                                                    
                                                                                                                                                                                          
  Fluxo de dados                                                                                                                                                                          
                                                                                                                                                                                          
  [Hardware WiFi] → [task: wifi] → [task: tiT (LwIP)] → [sua aplicação]                                                                                                                   
                                                                                                                                                                                          
  1. A task wifi lida com o hardware WiFi                                                                                                                                                 
  2. A task tiT processa os pacotes TCP/IP                                                                                                                                                
  3. Sua aplicação (httpd, etc.) recebe os dados processados                                                                                                                              
                                                                                                                                                                                          
  Por que o nome "tiT"?                                                                                                                                                                   
                                                                                                                                                                                          
  O FreeRTOS limita o tamanho dos nomes de tasks (geralmente 16 caracteres). O nome original seria algo como tcpip_thread, mas é truncado para tiT.                                       
                                                                                                                                                                                          
  É uma task do sistema                                                                                                                                                                   
                                                                                                                                                                                          
  Você não criou esta task - ela é criada automaticamente pelo ESP-IDF quando você inicializa o WiFi/networking. É essencial para qualquer comunicação de rede no ESP32.                  
                                                                                                                                                                                          
  No seu caso, ela usa ~1% de CPU porque está processando as requisições HTTP da interface web.                                                                                           
                                                                                                                                                                                          
  Sources:                                                                                                                                                                                
  - https://sming.readthedocs.io/en/stable/_inc/Sming/Arch/Esp32/README.html                                                                                                              
  - https://github.com/espressif/esp-idf/issues/2233                                                                                                                                      
                                                         