Fiz uma revisão detalhada do manual do mightyZAP e do código atual do projeto sobre a sincronização de 2 atuadores. Aqui estão as nossas opções e como podemos garantir essa sincronização:

1. Sincronização via Modbus Broadcast (Movimento 100% simultâneo)
De acordo com a Página 16 do manual, o ID = 0 funciona como um Broadcasting ID. Qualquer comando enviado para o ID 0 será processado por todos os atuadores conectados no barramento ao mesmo tempo.

Vantagem: Os motores vão começar a se mover exatamente no mesmo microssegundo.
Desvantagem: Pelo padrão Modbus, as requisições de Broadcast não têm resposta de confirmação (senão haveria colisão no barramento com todos tentando responder ao mesmo tempo). Logo, não teríamos a resposta imediata de sucesso. Se você tem apenas os 2 motores que precisam estar sintonizados no barramento (ou quer que todos se movam para o mesmo valor), essa é a forma de conseguir sincronia elétrica perfeita.
2. Sincronização Sequencial Rápida (Quase simultâneo)
Reparei que o código local já possui algumas funções (como mightyzap_sync_move_start) que fazem um pseudo-sincronismo. A biblioteca envia comandos FC06 em um loop (ex: Motor 1, depois Motor 2).

Em 57600 bps, cada comando Modbus leva cerca de 2 a 3 milissegundos para ser enviado e confirmado. Portanto, haverá um atraso de uns ~3ms a ~5ms entre o início do eixo 1 e do eixo 2. Na maioria das aplicações pneumáticas ou mecânicas, 5ms é imperceptível, então é o mais comum de se usar caso você precise checar os retornos.
3. Sincronização Mecânica / Calibração (Página 23 do manual)
Para garantir que, mesmo enviando o comando juntos, eles parem exatamente no mesmo ponto (em caso de imperfeições mecânicas), o mightyZAP oferece os registros de Min Position Calibration e Max Position Calibration. Eles servem justamente para:

"sincronizar as posições de início e fim de servos ligeiramente diferentes para o mesmo valor de Goal Position, corrigindo o erro de cada um." Isso significa que você pode dizer para mandá-los para "2000", e internamente eles compensam os décimos de milímetros se um estiver ligeiramente diferente do outro mecanicamente.

O que recomendo para o seu projeto: Se os dois motores sempre precisarem se mover juntos para a mesma posição e sustentam a mesma grelha/perfil, podemos criar uma função na nossa fila actuator_task que simplesmente escreve no ID 0, movendo os dois perfeitamente juntos em um único comando Modbus. Caso contrário, podemos enviar o comando pra um e pro outro rapidamente em loop.

Qual dessas abordagens atende melhor à sua mecânica? Prefere que testemos o comando de Broadcast (ID 0) na nossa API?