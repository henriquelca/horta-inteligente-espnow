# MACs das placas (só para identificação)

A rede usa broadcast com flooding, então **nenhum firmware precisa de MAC configurado**.
Esta tabela serve só para reconhecer as placas: o `id` no painel é o MAC do sensor e o `rep`
é o MAC de quem entregou a mensagem ao Gateway A. Se trocarem de placa, é só atualizar aqui.

| Papel | MAC | Firmware | Observação |
|---|---|---|---|
| Sensor 1 | 88:57:21:8E:10:F0 | 01_sensor (simulado) | USB CP210x (/dev/ttyUSB*) |
| Repetidor 1 | EC:64:C9:86:3E:CC | 02_repetidor | USB CH9102 (/dev/ttyACM*) |
| Gateway A (receptor) | EC:64:C9:85:B7:AC | 03_gateway_A_receptor | |
| Gateway B (MQTT) | — | 04_gateway_B_mqtt | |
