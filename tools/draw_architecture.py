#!/usr/bin/env python3
"""Draw exact vector explanations of Ledger's implemented memory/FIFO semantics.

These are mechanism diagrams, not measurements. No generated raster assets,
timing claims, ABI-dependent byte sizes, or external image dependencies.
"""
from __future__ import annotations

import argparse
from html import escape
import json
from pathlib import Path

TEAL, SLATE, CORAL = "#087f8c", "#64748b", "#e07a5f"
INK, LIGHT, LINE = "#1e293b", "#f8fafc", "#d5dee5"


class SVG:
    def __init__(self, width, height, title, description):
        self.width, self.height = width, height
        self.parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
            f'<title id="title">{escape(title)}</title><desc id="desc">{escape(description)}</desc>',
            '<defs><marker id="arrow" markerWidth="9" markerHeight="9" refX="7" refY="4.5" orient="auto"><path d="M0 0 L9 4.5 L0 9 Z" fill="#64748b"/></marker></defs>',
            f'<rect width="{width}" height="{height}" fill="white"/>',
            '<g font-family="DejaVu Sans,Arial,sans-serif" fill="#1e293b">']

    def text(self, x, y, value, size=16, fill=INK, weight="normal", anchor="start"):
        for number, line in enumerate(value.split("\n")):
            self.parts.append(f'<text x="{x}" y="{y+number*size*1.45}" font-size="{size}" font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{escape(line)}</text>')

    def box(self, x, y, width, height, title, body="", accent=TEAL, fill=LIGHT, title_size=18, body_size=14):
        self.parts.append(f'<rect x="{x}" y="{y}" width="{width}" height="{height}" rx="10" fill="{fill}" stroke="{LINE}"/>')
        self.parts.append(f'<rect x="{x}" y="{y}" width="5" height="{height}" rx="2" fill="{accent}"/>')
        self.text(x+18,y+29,title,title_size,weight="bold")
        if body:
            self.text(x+18,y+58,body,body_size,fill="#475569")

    def line(self, points, dashed=False, color=SLATE, arrow=True):
        coords=" ".join(f"{x},{y}" for x,y in points)
        dash=' stroke-dasharray="5 5"' if dashed else ""
        marker=' marker-end="url(#arrow)"' if arrow else ""
        self.parts.append(f'<polyline points="{coords}" fill="none" stroke="{color}" stroke-width="1.8"{dash}{marker}/>')

    def rule(self, y):
        self.line([(40,y),(1160,y)],arrow=False,color=LINE)

    def save(self, path):
        self.parts.extend(["</g>","</svg>"])
        Path(path).write_text("\n".join(self.parts)+"\n",encoding="utf-8")


def architecture(path):
    svg=SVG(1200,1000,"Ledger: core path and bounded memory", "Exact implementation diagram. Construction owns the allocations; process mutates preallocated storage and emits into caller storage. Timing boundaries exclude ingestion, snapshots and consumption.")
    svg.text(40,52,"Core path and bounded memory",32,weight="bold")
    svg.text(40,86,"Mechanism diagram · single book owner · integer price ticks · C++23",16,fill=SLATE)
    svg.text(40,139,"01  ONE COMMAND, ONE MUTATION BOUNDARY",15,fill=TEAL,weight="bold")
    svg.box(40,163,208,119,"Input Command","sequence, ID, action\nside, price, quantity, TIF",accent=SLATE)
    svg.box(281,163,254,119,"Validate before mutation","Output capacity; fields; ID\ncapacity; FOK availability",accent=TEAL)
    svg.box(568,163,267,119,"Choose the operation","Cancel: unlink live slot\nReplace: validated cancel-replace",accent=TEAL)
    svg.box(868,163,292,119,"Match crossing orders","Best opposite price first\nOldest maker at that price",accent=CORAL)
    svg.line([(248,222),(277,222)]);svg.line([(535,222),(564,222)]);svg.line([(835,222),(864,222)])
    svg.box(281,331,254,113,"Disposition of residual","GTC limit: append to tail\nIOC / market: expire residual",accent=TEAL)
    svg.box(568,331,267,113,"Caller EventBuffer","Accepted / trade / fill reports\nCancelled / expired / rejected",accent=CORAL)
    svg.box(868,331,292,113,"Applied / Rejected","OutputFull: no mutation\nConsumption occurs after process",accent=SLATE)
    svg.line([(1001,282),(1001,306),(408,306),(408,327)])
    svg.line([(701,282),(701,327)])
    svg.text(716,322,"cancel",12,fill=SLATE)
    svg.line([(535,387),(564,387)]);svg.line([(835,387),(864,387)])
    svg.line([(40,468),(1160,468)],dashed=True,arrow=False,color=TEAL)
    svg.text(40,496,"02  CONSTRUCTION OWNS STORAGE; PROCESS REUSES IT",15,fill=TEAL,weight="bold")
    svg.box(40,522,349,181,"OrderPool · N fixed slots","Node: ID, price, quantity, side\nprevious slot, next slot, live flag\nIntrusive queues + intrusive free list\nacquire / release never allocate",accent=TEAL)
    svg.box(424,522,351,181,"IdIndex · open addressing","Entry: complete 64-bit ID → slot\nPower-of-two capacity ≥ 2 × N\nMaximum live load factor: 1/2\nBackward-shift deletion heals gaps",accent=SLATE)
    svg.box(810,522,350,181,"Two price-level arrays","L = max_price − min_price + 1\nPer level: head, tail, units, orders\nBids and asks each own L levels\nFIFO links point into OrderPool",accent=CORAL)
    svg.box(40,744,349,128,"Two occupancy bitmaps","ceil(L / 64) words per side\nOne bit per nonempty price level\nBest price cache maintained on change",accent=TEAL,body_size=13)
    svg.box(424,744,351,128,"No growth in the timed call","process(Command, EventBuffer)\nNo snapshot construction or callbacks\nSingle-writer mutation; explicit ownership",accent=SLATE,body_size=13)
    svg.box(810,744,350,128,"Caller report storage","Required capacity ≥ 3 × N + 4 events\nChecked before any mutation\nNo event-vector growth in process",accent=CORAL,body_size=13)
    svg.rule(916)
    svg.text(40,947,"N = configured maximum live orders; formulas show capacities, not ABI-dependent byte sizes.",14,fill=SLATE)
    svg.text(40,973,"Rejected commands skip mutation; cancel skips matching. Diagnostic snapshots and invariant checks may allocate.",14,fill=SLATE)
    svg.save(path)


def fifo(path):
    svg=SVG(1200,1015,"Ledger: price-time priority worked trade", "An illustrative exact worked trade: resting asks A4, B6, C3 at 100 ticks; incoming buy IOC8 at 100. Fill A4 then B4; B2 and C3 remain. Two trades, one removed maker, zero emptied price levels, seven events.")
    svg.text(40,52,"Price first. FIFO within the price.",32,weight="bold")
    svg.text(40,86,"Worked example · illustrative state, not a captured performance measurement",16,fill=SLATE)
    svg.text(40,137,"BEFORE · BEST ASK = 100 TICKS",15,fill=TEAL,weight="bold")
    svg.box(40,164,215,130,"Ask level 100","units = 13 · orders = 3\nhead → A · tail → C\noccupancy bit = 1",accent=SLATE)
    for x,letter,identifier,quantity in ((302,"A",101,4),(593,"B",102,6),(884,"C",103,3)):
        svg.box(x,164,276,130,f"{letter} · ID {identifier}",f"sell {quantity} units @ 100 ticks\nFIFO arrival: {'first' if letter=='A' else 'second' if letter=='B' else 'third'}\nprev / next are pool-slot indices",accent=TEAL)
    svg.line([(255,229),(298,229)]);svg.line([(578,229),(589,229)]);svg.line([(869,229),(880,229)])
    svg.box(40,337,1119,87,"Incoming order · ID 104","Buy LIMIT / IOC · 8 units @ 100 ticks · crosses the resting asks · execution price is the maker price",accent=CORAL,body_size=15)
    svg.text(40,471,"MATCHING · ALWAYS TAKE THE CURRENT HEAD",15,fill=TEAL,weight="bold")
    svg.box(40,493,532,139,"Fill 1 · maker A","Trade 4 units @ 100 · A remaining = 0\nIncoming remaining = 4 · unlink A, return its slot\nLevel now: B6 → C3 · 9 units · 2 orders",accent=CORAL,body_size=15)
    svg.box(618,493,541,139,"Fill 2 · maker B","Trade 4 units @ 100 · B remaining = 2\nIncoming remaining = 0 · B keeps its FIFO position\nLevel now: B2 → C3 · 5 units · 2 orders",accent=CORAL,body_size=15)
    svg.line([(572,561),(614,561)])
    svg.text(40,679,"AFTER · NO PRICE LEVEL EMPTIED",15,fill=TEAL,weight="bold")
    svg.box(40,701,215,126,"Ask level 100","units = 5 · orders = 2\nhead → B · tail → C\noccupancy bit stays 1",accent=SLATE)
    svg.box(302,701,276,126,"B · ID 102","sell 2 units @ 100 ticks\nStill the oldest live maker\nnext → C",accent=TEAL)
    svg.box(620,701,276,126,"C · ID 103","sell 3 units @ 100 ticks\nNo fill; priority unchanged\nnext → no_slot",accent=TEAL)
    svg.line([(255,763),(298,763)]);svg.line([(578,763),(616,763)])
    svg.text(928,744,"2 trades",23,fill=CORAL,weight="bold")
    svg.text(928,776,"8 units executed",15,fill=SLATE)
    svg.text(928,804,"0 levels emptied",15,fill=SLATE)
    svg.rule(865)
    svg.text(40,899,"Seven reports for the incoming command",18,weight="bold")
    svg.text(40,931,"Accepted(104)  →  Trade(104,101,4)  →  Filled(101)  →  PartiallyFilled(104)",15,fill=SLATE)
    svg.text(40,960,"→  Trade(104,102,4)  →  PartiallyFilled(102)  →  Filled(104)",15,fill=SLATE)
    svg.text(40,995,"Replace is cancel-replace and always loses time priority, including a size reduction. FOK rejects before mutation if liquidity is insufficient.",13,fill=SLATE)
    svg.save(path)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outdir",type=Path,required=True)
    args=parser.parse_args(argv)
    args.outdir.mkdir(parents=True,exist_ok=True)
    paths=[args.outdir/"17-core-memory-contract.svg",args.outdir/"18-fifo-worked-trade.svg"]
    architecture(paths[0]);fifo(paths[1])
    print(json.dumps({"diagrams":[str(path) for path in paths]}))


if __name__=="__main__":
    main()
