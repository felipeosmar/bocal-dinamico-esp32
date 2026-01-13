● O que são as tasks "ipc0" e "ipc1"?                                                                                                                                                     
                                                                                                                                                                                          
  "ipc0" e "ipc1" são as tasks de Inter-Processor Communication (Comunicação Entre Processadores) do ESP32.                                                                               
                                                                                                                                                                                          
  Função                                                                                                                                                                                  
                                                                                                                                                                                          
  O ESP32 possui dois cores (Core 0 e Core 1) que executam em paralelo. As tasks IPC permitem que um core execute código no outro core de forma sincronizada.                             
                                                                                                                                                                                          
  | Task | Core   | Função                                         |                                                                                                                      
  |------|--------|------------------------------------------------|                                                                                                                      
  | ipc0 | Core 0 | Recebe e executa chamadas enviadas pelo Core 1 |                                                                                                                      
  | ipc1 | Core 1 | Recebe e executa chamadas enviadas pelo Core 0 |                                                                                                                      
                                                                                                                                                                                          
  Como funciona                                                                                                                                                                           
                                                                                                                                                                                          
  [Core 0] ──── precisa executar algo no Core 1 ────→ [ipc1 acorda e executa]                                                                                                             
  [Core 1] ──── precisa executar algo no Core 0 ────→ [ipc0 acorda e executa]                                                                                                             
                                                                                                                                                                                          
  Por que são necessárias?                                                                                                                                                                
                                                                                                                                                                                          
  Alguns recursos de hardware do ESP32 são afinados a um core específico e só podem ser acessados por código rodando naquele core:                                                        
                                                                                                                                                                                          
  - Certas interrupções                                                                                                                                                                   
  - Registradores específicos de CPU                                                                                                                                                      
  - Operações de cache                                                                                                                                                                    
  - Configurações de watchdog por core                                                                                                                                                    
                                                                                                                                                                                          
  Exemplo de uso interno                                                                                                                                                                  
                                                                                                                                                                                          
  // Código rodando no Core 1 quer desabilitar interrupções no Core 0                                                                                                                     
  esp_ipc_call(0, disable_interrupts_on_core, NULL);                                                                                                                                      
  // A task ipc0 acorda, executa disable_interrupts_on_core, e volta a dormir                                                                                                             
                                                                                                                                                                                          
  Por que estão "Suspended" com 0% CPU?                                                                                                                                                   
                                                                                                                                                                                          
  As tasks IPC ficam suspensas (dormindo) na maior parte do tempo. Elas só acordam quando há uma chamada IPC pendente, executam rapidamente, e voltam a dormir.                           
                                                                                                                                                                                          
  - Prioridade 24 (máxima) - Para garantir execução imediata quando necessário                                                                                                            
  - 0% CPU - Normal, pois ficam suspensas aguardando chamadas                                                                                                                             
  - ~2 KB Stack - Pequeno, pois apenas repassam chamadas                                                                                                                                  
                                                                                                                                                                                          
  Resumo                                                                                                                                                                                  
                                                                                                                                                                                          
  São tasks internas do ESP-IDF essenciais para o funcionamento do sistema dual-core, permitindo sincronização e execução de código entre os dois processadores. Você não interage diretamente com elas - o sistema as utiliza internamente quando necessário.