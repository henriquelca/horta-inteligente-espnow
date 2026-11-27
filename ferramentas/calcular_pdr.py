#!/usr/bin/env python3
"""
Calcula o Packet Delivery Ratio (PDR) a partir do log CSV do Node-RED.

Uso:
    python3 calcular_pdr.py ~/cps_log_equipeX.csv            # todas as rodadas
    python3 calcular_pdr.py ~/cps_log_equipeX.csv --total 1000
    python3 calcular_pdr.py LOG.csv --total 1000 --csv resultado.csv

Como as rodadas são separadas: cada vez que o contador de um sensor VOLTA
(ex.: 1000 -> 1, quando você aperta BOOT para a próxima distância), começa
uma nova rodada. Anote a distância de cada rodada na ordem em que fez.

PDR = recebidas (sem duplicatas) / enviadas
  enviadas = --total (se informado; padrão do teste = 1000)
             senão  = maior contador - menor contador + 1
"""
import argparse
import csv
import sys
from datetime import datetime


def ler(caminho):
    with open(caminho, newline="", encoding="utf-8") as f:
        for linha in csv.reader(f):
            if not linha or linha[0] == "ts_iso":     # cabeçalho (pode repetir após deploy)
                continue
            try:
                yield {"ts": linha[0], "id": linha[2], "cont": int(linha[3]),
                       "hop": linha[6] if len(linha) > 6 else "", "rssi": linha[8] if len(linha) > 8 else ""}
            except (ValueError, IndexError):
                continue


def rodadas(registros):
    por_sensor = {}
    for r in registros:
        lst = por_sensor.setdefault(r["id"], [])
        if not lst or r["cont"] < lst[-1]["ultimo"]:
            lst.append({"id": r["id"], "inicio": r["ts"], "fim": r["ts"], "conts": set(),
                        "ultimo": r["cont"], "rssi": []})
        rd = lst[-1]
        rd["conts"].add(r["cont"])
        rd["ultimo"] = r["cont"]
        rd["fim"] = r["ts"]
        if r["rssi"] not in ("", "0"):
            rd["rssi"].append(int(r["rssi"]))
    return por_sensor


def hora(ts):
    try:
        return datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone().strftime("%d/%m %H:%M:%S")
    except ValueError:
        return ts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--total", type=int, help="mensagens enviadas por rodada (ex.: 1000)")
    ap.add_argument("--min", type=int, default=20, help="ignora rodadas com menos msgs recebidas (padrão 20)")
    ap.add_argument("--csv", help="salva a tabela de resultados neste arquivo")
    a = ap.parse_args()

    resultado = []
    for sid, lst in rodadas(ler(a.log)).items():
        n = 0
        for rd in lst:
            recebidas = len(rd["conts"])
            if recebidas < a.min:
                continue
            n += 1
            if a.total:
                enviadas = a.total
                recebidas = len([c for c in rd["conts"] if 1 <= c <= a.total])
            else:
                enviadas = max(rd["conts"]) - min(rd["conts"]) + 1
            rssi = round(sum(rd["rssi"]) / len(rd["rssi"]), 1) if rd["rssi"] else ""
            resultado.append({"sensor": sid, "rodada": n, "inicio": hora(rd["inicio"]), "fim": hora(rd["fim"]),
                              "enviadas": enviadas, "recebidas": recebidas, "perdidas": enviadas - recebidas,
                              "pdr_%": round(100 * recebidas / enviadas, 2) if enviadas else 0,
                              "rssi_medio_dBm": rssi})

    if not resultado:
        print("Nenhuma rodada encontrada no log.")
        return 1

    cols = list(resultado[0].keys())
    larg = {c: max(len(c), *(len(str(r[c])) for r in resultado)) for c in cols}
    print("  ".join(c.ljust(larg[c]) for c in cols))
    print("  ".join("-" * larg[c] for c in cols))
    for r in resultado:
        print("  ".join(str(r[c]).ljust(larg[c]) for c in cols))

    if a.csv:
        with open(a.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=cols)
            w.writeheader()
            w.writerows(resultado)
        print(f"\nSalvo em {a.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
