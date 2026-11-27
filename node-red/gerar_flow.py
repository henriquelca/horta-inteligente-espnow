#!/usr/bin/env python3
"""Gera node-red/flow_cps.json. Uso: python3 gerar_flow.py [equipe] [broker] [porta]
Ex.: python3 gerar_flow.py equipe3 broker.hivemq.com 1883
(Você também pode só importar o flow_cps.json e trocar 'equipeX' nos nós MQTT.)"""
import json, sys, pathlib

EQUIPE = sys.argv[1] if len(sys.argv) > 1 else "equipeX"
BROKER = sys.argv[2] if len(sys.argv) > 2 else "broker.hivemq.com"
PORTA = sys.argv[3] if len(sys.argv) > 3 else "1883"

TAB = "cps_tab_flow"
CORES = ["#1f77b4", "#d62728", "#2ca02c", "#ff7f0e", "#9467bd",
         "#8c564b", "#e377c2", "#7f7f7f", "#17becf"]

FN_CARIMBO = r"""// Associa o horário de RECEPÇÃO (relógio do servidor Node-RED) a cada mensagem
const p = msg.payload;
if (typeof p !== 'object' || p === null || p.id === undefined) {
    node.warn('Mensagem sem id ignorada: ' + JSON.stringify(p));
    return null;
}
const agora = new Date();
p.ts_ms = agora.getTime();
p.ts = agora.toISOString();
p.hora = agora.toLocaleTimeString('pt-BR');
msg.sensor = String(p.id);
return msg;"""

FN_V = r"""// Uma série por sensor no gráfico (msg.topic = id do sensor)
const v = msg.payload.%s;
if (typeof v !== 'number') return null;
return { topic: msg.sensor, payload: v };"""

FN_CSV = r"""// Uma linha CSV por mensagem; o arquivo fica na pasta do usuário
const casa = env.get('HOME') || env.get('USERPROFILE') || '.';
const arquivo = casa + '/cps_log_%s.csv';
const p = msg.payload;
const campo = v => (v === undefined || v === null) ? '' : String(v);
const linhas = [];
if (!context.get('cabecalho')) {   // escreve o cabeçalho a cada (re)deploy
    context.set('cabecalho', true);
    linhas.push({ filename: arquivo, payload: 'ts_iso,ts_ms,id,cont,v1,v2,hop,rep,rssi' });
}
linhas.push({ filename: arquivo, payload: [p.ts, p.ts_ms, p.id, p.cont, p.v1, p.v2, p.hop, p.rep, p.rssi].map(campo).join(',') });
return [linhas];""" % EQUIPE

FN_TABELA = r"""// Estado por sensor: últimos valores + estatística de entrega (PDR) ao vivo
let s = flow.get('sensores') || {};
if (msg.reset) { s = {}; flow.set('sensores', s); }
else if (!msg.tick) {
    const p = msg.payload, id = msg.sensor;
    let e = s[id];
    // sensor novo ou contador reiniciado (reset do nó / nova rodada de teste)
    if (!e || p.cont < e.ultimoCont) e = { id: id, primeiroCont: p.cont, recebidas: 0, ultimoCont: p.cont - 1 };
    if (p.cont !== e.ultimoCont) e.recebidas++;           // ignora duplicatas
    Object.assign(e, { ultimoCont: p.cont, v1: p.v1, v2: p.v2, hop: p.hop,
                       rep: p.rep, rssi: p.rssi, hora: p.hora, ts_ms: p.ts_ms });
    e.esperadas = e.ultimoCont - e.primeiroCont + 1;
    e.perdidas = e.esperadas - e.recebidas;
    e.pdr = e.esperadas > 0 ? 100 * e.recebidas / e.esperadas : 100;
    s[id] = e;
    flow.set('sensores', s);
}
const agora = Date.now();
msg.payload = Object.values(s)
    .map(e => Object.assign({}, e, { idade: Math.round((agora - e.ts_ms) / 1000) }))
    .sort((a, b) => a.id.localeCompare(b.id));
return msg;"""

FN_STATUS = r"""// LWT do gateway: "online" ao conectar, "offline" publicado pelo broker se ele cair
const txt = String(msg.payload).trim().toLowerCase();
msg.payload = { online: txt === 'online', texto: txt, hora: new Date().toLocaleString('pt-BR') };
flow.set('gateway_online', msg.payload.online);
return msg;"""

TPL_TABELA = r"""<div class="cps-wrap">
<table class="cps">
  <tr><th>Sensor (MAC)</th><th>Temperatura (°C)<br><small>v1</small></th><th>Umidade (%)<br><small>v2</small></th>
      <th>Contador</th><th>Hop</th><th>RSSI</th><th>Recebidas / Esperadas</th><th>PDR</th><th>Última msg</th></tr>
  <tr ng-repeat="s in msg.payload" ng-class="{'cps-velho': s.idade > 10}">
    <td><b>{{s.id}}</b><br><small>via {{s.rep}}</small></td>
    <td class="cps-num">{{s.v1 === null ? '—' : (s.v1 | number:1)}}</td>
    <td class="cps-num">{{s.v2 === null ? '—' : (s.v2 | number:1)}}</td>
    <td class="cps-num">{{s.ultimoCont}}</td>
    <td class="cps-num">{{s.hop}}</td>
    <td class="cps-num">{{s.rssi}} dBm</td>
    <td class="cps-num">{{s.recebidas}} / {{s.esperadas}}</td>
    <td class="cps-num"><b>{{s.pdr | number:1}}%</b></td>
    <td>{{s.hora}}<br><small>há {{s.idade}} s</small></td>
  </tr>
</table>
<p ng-if="!msg.payload || !msg.payload.length">Aguardando mensagens dos sensores...</p>
</div>
<style>
.cps-wrap { overflow-x: auto; }
.cps { width: 100%; border-collapse: collapse; font-size: 14px; }
.cps th, .cps td { padding: 6px 8px; border-bottom: 1px solid rgba(128,128,128,.35); text-align: left; }
.cps th { font-weight: 600; }
.cps-num { text-align: right !important; font-variant-numeric: tabular-nums; }
.cps-velho td { opacity: .45; }
</style>"""

TPL_STATUS = r"""<div class="gw">
  <span class="gw-led" ng-class="{'on': msg.payload.online, 'off': msg.payload && !msg.payload.online}"></span>
  <span class="gw-txt">{{ !msg.payload ? 'Aguardando...' : (msg.payload.online ? 'ONLINE' : 'OFFLINE') }}</span>
  <div class="gw-hora" ng-if="msg.payload">desde {{msg.payload.hora}}</div>
</div>
<style>
.gw { display:flex; align-items:center; gap:12px; flex-wrap:wrap; font-size: 22px; font-weight: 700; }
.gw-led { width: 26px; height: 26px; border-radius: 50%; background: #888; }
.gw-led.on  { background: #2ecc40; box-shadow: 0 0 12px #2ecc40; }
.gw-led.off { background: #ff4136; box-shadow: 0 0 12px #ff4136; }
.gw-hora { width:100%; font-size: 13px; font-weight: 400; opacity: .7; }
</style>"""


def chart(nid, nome, label, grupo, ordem):
    return {"id": nid, "type": "ui_chart", "z": TAB, "name": nome, "group": grupo, "order": ordem,
            "width": "12", "height": "6", "label": label, "chartType": "line", "legend": "true",
            "xformat": "HH:mm:ss", "interpolate": "linear", "nodata": "Sem dados ainda", "dot": False,
            "ymin": "", "ymax": "", "removeOlder": "1", "removeOlderPoints": "2000",
            "removeOlderUnit": "3600", "cutout": 0, "useOneColor": False, "useUTC": False,
            "colors": CORES, "outputs": 1, "useDifferentColor": False, "className": "",
            "x": 900, "y": 60 if nid.endswith("v1") else 120, "wires": [[]]}


def fn(nid, nome, codigo, x, y, wires, outputs=1):
    return {"id": nid, "type": "function", "z": TAB, "name": nome, "func": codigo,
            "outputs": outputs, "timeout": 0, "noerr": 0, "initialize": "", "finalize": "",
            "libs": [], "x": x, "y": y, "wires": wires}


nodes = [
    {"id": TAB, "type": "tab", "label": f"CPS - Rede ESP-NOW ({EQUIPE})", "disabled": False, "info": ""},
    {"id": "cps_broker", "type": "mqtt-broker", "name": f"Broker {BROKER}", "broker": BROKER,
     "port": str(PORTA), "clientid": "", "autoConnect": True, "usetls": False, "protocolVersion": "4",
     "keepalive": "60", "cleansession": True, "autoUnsubscribe": True,
     "birthTopic": "", "birthQos": "0", "birthPayload": "", "birthMsg": {},
     "closeTopic": "", "closeQos": "0", "closePayload": "", "closeMsg": {},
     "willTopic": "", "willQos": "0", "willPayload": "", "willMsg": {}, "userProps": "", "sessionExpiry": ""},
    {"id": "cps_ui_tab", "type": "ui_tab", "name": f"CPS {EQUIPE}", "icon": "dashboard", "order": 1,
     "disabled": False, "hidden": False},
    {"id": "cps_g_status", "type": "ui_group", "name": "Gateway", "tab": "cps_ui_tab", "order": 1,
     "disp": True, "width": "6", "collapse": False, "className": ""},
    {"id": "cps_g_sens", "type": "ui_group", "name": "Sensores em tempo real", "tab": "cps_ui_tab",
     "order": 2, "disp": True, "width": "18", "collapse": False, "className": ""},
    {"id": "cps_g_graf", "type": "ui_group", "name": "Histórico", "tab": "cps_ui_tab", "order": 3,
     "disp": True, "width": "12", "collapse": False, "className": ""},

    # ---------- telemetria ----------
    {"id": "cps_in_sens", "type": "mqtt in", "z": TAB, "name": "Sensores",
     "topic": f"cps/{EQUIPE}/sensor/+", "qos": "0", "datatype": "utf8", "broker": "cps_broker",
     "nl": False, "rap": True, "rh": 0, "inputs": 0, "x": 110, "y": 100, "wires": [["cps_json"]]},
    {"id": "cps_json", "type": "json", "z": TAB, "name": "JSON -> objeto", "property": "payload",
     "action": "obj", "pretty": False, "x": 290, "y": 100, "wires": [["cps_fn_ts"]]},
    fn("cps_fn_ts", "Carimbo de tempo (servidor)", FN_CARIMBO, 500, 100,
       [["cps_fn_v1", "cps_fn_v2", "cps_fn_csv", "cps_fn_tab", "cps_debug"]]),
    fn("cps_fn_v1", "v1 por sensor", FN_V % "v1", 720, 60, [["cps_chart_v1"]]),
    fn("cps_fn_v2", "v2 por sensor", FN_V % "v2", 720, 120, [["cps_chart_v2"]]),
    chart("cps_chart_v1", "Gráfico v1", "Temperatura (°C) — v1", "cps_g_graf", 1),
    chart("cps_chart_v2", "Gráfico v2", "Umidade (%) — v2", "cps_g_graf", 2),
    fn("cps_fn_csv", "Linha CSV", FN_CSV, 720, 180, [["cps_file"]]),
    {"id": "cps_file", "type": "file", "z": TAB, "name": "Log em arquivo (~/cps_log_*.csv)",
     "filename": "", "appendNewline": True, "createDir": True, "overwriteFile": "false",
     "encoding": "utf8", "x": 960, "y": 180, "wires": [[]]},
    fn("cps_fn_tab", "Tabela + PDR por sensor", FN_TABELA, 740, 240, [["cps_tpl_tab"]]),
    {"id": "cps_tpl_tab", "type": "ui_template", "z": TAB, "group": "cps_g_sens", "name": "Tabela sensores",
     "order": 1, "width": "18", "height": "5", "format": TPL_TABELA, "storeOutMessages": True,
     "fwdInMessages": False, "resendOnRefresh": True, "templateScope": "local", "className": "",
     "x": 980, "y": 240, "wires": [[]]},
    {"id": "cps_tick", "type": "inject", "z": TAB, "name": "a cada 2 s", "props": [{"p": "tick", "v": "true", "vt": "bool"}],
     "repeat": "2", "crontab": "", "once": True, "onceDelay": "1", "topic": "", "x": 500, "y": 280,
     "wires": [["cps_fn_tab"]]},
    {"id": "cps_btn_reset", "type": "ui_button", "z": TAB, "name": "Zerar PDR", "group": "cps_g_sens",
     "order": 2, "width": "4", "height": "1", "passthru": False, "label": "Zerar estatísticas",
     "tooltip": "Zera contagem de recebidas/PDR (o arquivo de log não é apagado)", "color": "", "bgcolor": "",
     "className": "", "icon": "refresh", "payload": "", "payloadType": "str", "topic": "reset", "topicType": "str",
     "x": 490, "y": 320, "wires": [["cps_fn_reset"]]},
    fn("cps_fn_reset", "marca reset", "msg.reset = true;\nreturn msg;", 660, 320, [["cps_fn_tab"]]),
    {"id": "cps_debug", "type": "debug", "z": TAB, "name": "debug", "active": False, "tosidebar": True,
     "console": False, "tostatus": False, "complete": "payload", "targetType": "msg", "statusVal": "",
     "statusType": "auto", "x": 710, "y": 20, "wires": []},

    # ---------- status do gateway (LWT) ----------
    {"id": "cps_in_status", "type": "mqtt in", "z": TAB, "name": "Status do gateway (LWT)",
     "topic": f"cps/{EQUIPE}/gateway/status", "qos": "1", "datatype": "utf8", "broker": "cps_broker",
     "nl": False, "rap": True, "rh": 0, "inputs": 0, "x": 150, "y": 400, "wires": [["cps_fn_status"]]},
    fn("cps_fn_status", "online/offline", FN_STATUS, 380, 400, [["cps_tpl_status"]]),
    {"id": "cps_tpl_status", "type": "ui_template", "z": TAB, "group": "cps_g_status", "name": "Indicador gateway",
     "order": 1, "width": "6", "height": "2", "format": TPL_STATUS, "storeOutMessages": True,
     "fwdInMessages": False, "resendOnRefresh": True, "templateScope": "local", "className": "",
     "x": 600, "y": 400, "wires": [[]]},
]

saida = pathlib.Path(__file__).with_name("flow_cps.json")
saida.write_text(json.dumps(nodes, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"Gerado {saida} (equipe={EQUIPE}, broker={BROKER}:{PORTA})")
