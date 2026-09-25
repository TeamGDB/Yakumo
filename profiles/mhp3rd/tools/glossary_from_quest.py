# Second batch: the quest/menu labels of table 2 and the monster/khezu names.
# Appended to the glossary; build_lang.py checks each row against the disc.
import csv, io, re

DUMP = r"C:\trabalios\istudous\romhacking\mhp3rd\Yakumo\docs\TEXT_DUMP\strings.csv"
GLOS = r"C:\trabalios\istudous\romhacking\mhp3rd\Yakumo\profiles\mhp3rd\translations\glossary.tsv"

rows = list(csv.DictReader(open(DUMP, encoding="utf-8")))
by_key = {}
for r in rows:
    by_key[(r["entry"], r["table"], r["index"])] = r["text"]

# table:index -> (pt, es)
T = {
    # quest result / supply
    (2, 146): ("Valor", "Valor"),
    (2, 147): ("Quantidade", "Cantidad"),
    (2, 150): ("Espaços necessários", "Espacios necesarios"),
    (2, 151): ("Tempo restante", "Tiempo restante"),
    (2, 152): ("Itens da missão", "Objetos de misión"),
    (2, 153): ("Subtotal", "Subtotal"),
    (2, 154): ("Pts Yukumo", "Pts Yukumo"),
    (2, 155): ("Total atual", "Total actual"),
    (2, 156): ("Total de Pts Yukumo", "Total de Pts Yukumo"),
    (2, 157): ("Descartar", "Descartar"),
    (2, 158): ("Dar", "Dar"),
    (2, 159): ("Entregar", "Entregar"),
    (2, 160): ("Trocar", "Intercambiar"),
    (2, 161): ("Trocar", "Intercambiar"),
    (2, 162): ("Descartar", "Descartar"),
    (2, 164): ("Sair", "Salir"),
    (2, 165): ("Lista de combos", "Lista de combos"),
    (2, 166): ("Lista de monstros", "Lista de monstruos"),
    (2, 167): ("Status do camarada", "Estado del camarada"),
    # monster-list fields
    (2, 176): ("Nome", "Nombre"),
    (2, 177): ("Alcunha", "Alias"),
    (2, 178): ("Tipo", "Tipo"),
    (2, 179): ("Risco", "Riesgo"),
    (2, 180): ("Nº (captura)", "N.º (captura)"),
    (2, 181): ("Maior", "Mayor"),
    (2, 182): ("Menor", "Menor"),
    # monster classes
    (2, 189): ("Lynian", "Lynian"),
    (2, 190): ("Neopteron", "Neopteron"),
    (2, 191): ("Wyvern Aquático", "Wyvern acuático"),
    (2, 192): ("Herbívoro", "Herbívoro"),
    (2, 193): ("Wyvern Ave", "Wyvern ave"),
    (2, 194): ("Besta Presa", "Bestia colmilluda"),
    (2, 195): ("Wyvern Voador", "Wyvern volador"),
    (2, 196): ("Wyvern Presa", "Wyvern colmilludo"),
    (2, 197): ("Wyvern Brutal", "Wyvern brutal"),
    # monster count messages
    (2, 126): ("Falta 1 monstro!", "¡Queda 1 monstruo!"),
    (2, 127): ("Faltam 2 monstros!", "¡Quedan 2 monstruos!"),
    (2, 128): ("Faltam 5 monstros!", "¡Quedan 5 monstruos!"),
    (2, 129): ("Faltam 10 monstros!", "¡Quedan 10 monstruos!"),
    (2, 130): ("Faltam 20 monstros!", "¡Quedan 20 monstruos!"),
    # supply / quest messages
    (2, 140): ("O navio foi destruído.", "El barco ha sido destruido."),
    (2, 96): ("Vender o que resta?\\nOK?　", "¿Vender lo restante?\\n¿OK?　"),
    (2, 102): ("~C02Baú de itens cheio.", "~C02Caja de objetos llena."),
    (2, 106): ("Vender o que resta?", "¿Vender lo restante?"),
    (2, 107): ("Vender o que resta?", "¿Vender lo restante?"),
    (2, 77): ("~B01Cancel.", "~B01Cancel."),
    (2, 81): ("~B06 Editar nome", "~B06 Nombre"),
}

# The monster names of table 2, from entry 308 on (docs/DEBUG_MENU.md).
MONSTERS = {
    308: "NO DATA", 309: "Rathian", 310: "Rathalos", 311: "Qurupeco", 312: "Gigginox",
    313: "Barioth", 314: "Barroth", 315: "Diablos", 316: "Bnahabra", 317: "Rhenoplos",
    318: "Aptonoth", 319: "Epioth", 320: "Popo", 321: "Anteka", 322: "Slagtoth",
    323: "Kelbi", 324: "Bullfango", 325: "Felyne", 326: "Melynx", 327: "Jaggi",
    328: "Jaggia", 329: "Great Jaggi", 330: "Baggi", 331: "Great Baggi", 332: "Wroggi",
    333: "Great Wroggi", 334: "Arzuros", 335: "Lagombi", 336: "Volvidon", 337: "Qurupeco",
    338: "Ludroth", 339: "Royal Ludroth", 340: "Gobul", 341: "Nibelsnarf", 342: "Agnaktor",
    343: "Glacial Agnaktor", 344: "Uragaan", 345: "Steel Uragaan", 346: "Duramboros",
    347: "Nargacuga", 348: "Green Nargacuga", 349: "Barioth", 350: "Sand Barioth",
    351: "Tigrex", 352: "Brute Tigrex", 353: "Diablos", 354: "Black Diablos",
    355: "Rathalos", 356: "Silver Rathalos", 357: "Rathian", 358: "Gold Rathian",
    359: "Zinogre", 360: "Stygian Zinogre", 361: "Alatreon", 362: "Amatsu",
    363: "Deviljho", 364: "Rajang", 365: "Yian Kut-Ku", 366: "Blue Yian Kut-Ku",
    367: "Gypceros", 368: "Purple Gypceros", 369: "Khezu", 370: "Red Khezu",
    371: "Basarios", 372: "Ruby Basarios", 373: "Gravios", 374: "Black Gravios",
    375: "Kirin", 376: "Oroshi Kirin", 377: "Plesioth", 378: "Green Plesioth",
    379: "Chameleos", 380: "Teostra", 381: "Lunastra", 382: "Kushala Daora",
    383: "Rusted Kushala Daora", 384: "Akantor", 385: "Ukanlos", 386: "Fatalis",
}
# These keep their names in both languages; Jaggia/Aptonoth etc. have standard
# Portuguese forms. Only where a clear one exists.
MONSTER_PT = {
    "Rathian": "Rathian", "Rathalos": "Rathalos", "Qurupeco": "Qurupeco", "Gigginox": "Gigginox",
    "Barioth": "Barioth", "Barroth": "Barroth", "Diablos": "Diablos", "Jaggi": "Jaggi",
    "Jaggia": "Jaggia", "Great Jaggi": "Grande Jaggi", "Great Baggi": "Grande Baggi",
    "Great Wroggi": "Grande Wroggi", "Arzuros": "Arzuros", "Lagombi": "Lagombi",
    "Volvidon": "Volvidon", "Royal Ludroth": "Ludroth Real", "Gobul": "Gobul",
    "Nibelsnarf": "Nibelsnarf", "Agnaktor": "Agnaktor", "Glacial Agnaktor": "Agnaktor Glacial",
    "Uragaan": "Uragaan", "Steel Uragaan": "Uragaan de Aço", "Duramboros": "Duramboros",
    "Nargacuga": "Nargacuga", "Green Nargacuga": "Nargacuga Verde", "Sand Barioth": "Barioth da Areia",
    "Tigrex": "Tigrex", "Brute Tigrex": "Tigrex Brutal", "Black Diablos": "Diablos Negra",
    "Silver Rathalos": "Rathalos Prateado", "Gold Rathian": "Rathian Dourada",
    "Zinogre": "Zinogre", "Stygian Zinogre": "Zinogre Infernal", "Alatreon": "Alatreon",
    "Amatsu": "Amatsu", "Deviljho": "Deviljho", "Rajang": "Rajang",
    "Yian Kut-Ku": "Yian Kut-Ku", "Blue Yian Kut-Ku": "Yian Kut-Ku Azul",
    "Gypceros": "Gypceros", "Purple Gypceros": "Gypceros Roxo", "Khezu": "Khezu",
    "Red Khezu": "Khezu Vermelho", "Basarios": "Basarios", "Ruby Basarios": "Basarios Rubi",
    "Gravios": "Gravios", "Black Gravios": "Gravios Negro", "Kirin": "Kirin",
    "Oroshi Kirin": "Kirin Oroshi", "Plesioth": "Plesioth", "Green Plesioth": "Plesioth Verde",
    "Chameleos": "Chameleos", "Teostra": "Teostra", "Lunastra": "Lunastra",
    "Kushala Daora": "Kushala Daora", "Rusted Kushala Daora": "Kushala Daora Enferrujado",
    "Akantor": "Akantor", "Ukanlos": "Ukanlos", "Fatalis": "Fatalis",
    "Bullfango": "Bullfango", "Kelbi": "Kelbi", "Slagtoth": "Slagtoth", "Anteka": "Anteka",
    "Popo": "Popo", "Epioth": "Epioth", "Aptonoth": "Aptonoth", "Rhenoplos": "Rhenoplos",
    "Bnahabra": "Bnahabra", "Felyne": "Felyne", "Melynx": "Melynx", "Baggi": "Baggi",
    "Wroggi": "Wroggi", "Ludroth": "Ludroth",
}


def monster_es(name):
    # Spanish keeps most monster names; a few have a standard translation.
    special = {"Royal Ludroth": "Ludroth Real", "Glacial Agnaktor": "Agnaktor Glacial",
               "Steel Uragaan": "Uragaan de acero", "Green Nargacuga": "Nargacuga verde",
               "Sand Barioth": "Barioth de arena", "Brute Tigrex": "Tigrex brutal",
               "Black Diablos": "Diablos negra", "Silver Rathalos": "Rathalos plateado",
               "Gold Rathian": "Rathian dorada", "Stygian Zinogre": "Zinogre estigio",
               "Blue Yian Kut-Ku": "Yian Kut-Ku azul", "Purple Gypceros": "Gypceros morado",
               "Red Khezu": "Khezu rojo", "Ruby Basarios": "Basarios rubí",
               "Black Gravios": "Gravios negro", "Oroshi Kirin": "Kirin oroshi",
               "Green Plesioth": "Plesioth verde", "Rusted Kushala Daora": "Kushala Daora oxidado",
               "Great Jaggi": "Gran Jaggi", "Great Baggi": "Gran Baggi", "Great Wroggi": "Gran Wroggi"}
    return special.get(name, name)


def add(lines, table, index, pt, es):
    en = by_key.get(("16", str(table), str(index)))
    if en is None:
        return False
    lines.append("16\t%d\t%d\t%s\t%s\t%s\n" % (table, index, en, pt, es))
    return True


lines = []
count = 0
for (table, index), (pt, es) in sorted(T.items()):
    if add(lines, table, index, pt, es):
        count += 1
for index, name in sorted(MONSTERS.items()):
    if name == "NO DATA":
        continue
    en = by_key.get(("16", "2", str(index)))
    if en is None:
        continue
    pt = MONSTER_PT.get(name, name)
    es = monster_es(name)
    lines.append("16\t2\t%d\t%s\t%s\t%s\n" % (index, en, pt, es))
    count += 1

with open(GLOS, "a", encoding="utf-8", newline="\n") as handle:
    handle.write("\n# --- quest menu and monster names (table 2) ---\n")
    for line in lines:
        handle.write(line)
print("rows added:", count)
