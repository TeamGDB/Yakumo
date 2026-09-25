# Generates translations/glossary.tsv from the extracted strings plus a
# dictionary of translations. English comes from the disc (via the dump), so a
# key can never drift from what the game holds.
import csv, sys, io, os

DUMP = r"C:\trabalios\istudous\romhacking\mhp3rd\Yakumo\docs\TEXT_DUMP\strings.csv"
OUT = r"C:\trabalios\istudous\romhacking\mhp3rd\Yakumo\profiles\mhp3rd\translations\glossary.tsv"

rows = list(csv.DictReader(open(DUMP, encoding="utf-8")))


def english(entry, table, index):
    for r in rows:
        if r["entry"] == str(entry) and r["table"] == str(table) and r["index"] == str(index):
            return r["text"]
    return None


# table:index -> (pt-BR, es). The English key is read back from the disc.
T = {
    # --- short options ---------------------------------------------------
    (2, 4): ("Qtd", "Cant."),
    (2, 18): ("Conf", "Conf"),
    (2, 20): ("Cancelar", "Cancelar"),
    (2, 21): ("Sim", "Sí"),
    (2, 22): ("Não", "No"),
    # --- button prompts --------------------------------------------------
    (2, 43): ("~B00Conf", "~B00Conf"),
    (2, 44): ("~B01Voltar", "~B01Atrás"),
    (2, 45): ("~B03Voltar", "~B03Atrás"),
    (2, 46): ("~B03~B01Voltar", "~B03~B01Atrás"),
    (2, 47): ("~B02Info", "~B02Info"),
    (2, 48): ("~B03Info. hab.", "~B03Info. hab."),
    (2, 49): ("~B03Info. equi.", "~B03Info. equi."),
    (2, 50): ("~B00Sel.", "~B00Sel."),
    (2, 51): ("~B00Desmarc.", "~B00Deselec."),
    (2, 52): ("~B06 Terminar", "~B06 Acabar"),
    (2, 53): ("~B07 Ordenar", "~B07 Ordenar"),
    (2, 54): ("~B06 Câmera", "~B06 Cámara"),
    (2, 55): ("~B08Girar", "~B08Girar"),
    (2, 56): ("~B09Câmera", "~B09Cámara"),
    (2, 57): ("~B06 Menu", "~B06 Menú"),
    (2, 58): ("~B01Salir", "~B01Salir"),
    (2, 59): ("~B00Cor", "~B00Color"),
    (2, 60): ("~B03Equip.", "~B03Equipo"),
    (2, 61): ("~B03~B00Conf", "~B03~B00Conf"),
    (2, 62): ("~B06 Nome", "~B06 Nombre"),
    (2, 63): ("~B06 Regist.", "~B06 Regist."),
    (2, 64): ("~B01Cancel.", "~B01Cancel."),
    (2, 65): ("~B07 Por nome", "~B07 Por nombre"),
    (2, 66): ("~B07 Por habil.", "~B07 Por habil."),
    (2, 67): ("~B07 Trocar", "~B07 Cambiar"),
    (2, 68): ("~B03Aproximar", "~B03Acercar"),
    (2, 69): ("~B03Afastar", "~B03Alejar"),
    (2, 70): ("~B02Sinal", "~B02Señal"),
    (2, 71): ("~B03Mover", "~B03Mover"),
    (2, 72): ("~B02Ordem", "~B02Orden"),
    (2, 73): ("~B02Ordenar", "~B02Ordenar"),
    (2, 74): ("~B07 Padrão", "~B07 Defecto"),
    (2, 75): ("~B02Qtd", "~B02Cant."),
    (2, 76): ("~B06 Iniciar", "~B06 Empezar"),
    (2, 78): ("~B06 Voltar", "~B06 Atrás"),
    (2, 79): ("~B03Trocar", "~B03Cambiar"),
    (2, 80): ("~B02Detalhes", "~B02Detalles"),
    (2, 82): ("~B03Mais", "~B03Más"),
    (2, 83): ("~B07 Ver", "~B07 Ver"),
    (2, 84): ("~B02Vender", "~B02Vender"),
    (2, 85): ("~B03Comparar", "~B03Comparar"),
    (2, 86): ("~B03Detalhes", "~B03Detalles"),
    (2, 87): ("~B03Ocultar arma", "~B03Ocultar arma"),
    (2, 88): ("~B03Mostrar arma", "~B03Mostrar arma"),
    (2, 89): ("~B01Desmarc.", "~B01Deselec."),
    (2, 90): ("~B03Info. hab.", "~B03Info. hab."),
    (2, 91): ("~B00Efeitos de tela", "~B00Efectos"),
    # --- messages --------------------------------------------------------
    (2, 95): ("Item trocado.", "Objeto intercambiado."),
    (2, 99): ("Vender o que resta e sair.", "Vender lo restante y salir."),
    (2, 105): ("Vender o que resta?", "¿Vender lo restante?"),
    (2, 113): ("Voltando à vila em 20 segundos", "Volviendo a la aldea en 20 segundos"),
    (2, 114): ("Voltando à vila em 1 minuto", "Volviendo a la aldea en 1 minuto"),
    (2, 115): ("Missão concluída!", "¡Misión completada!"),
    (2, 116): ("Recompensa reduzida em %dz", "Recompensa reducida en %dz"),
    (2, 117): ("Tempo restante: %dmin", "Tiempo restante: %dmin"),
    (2, 118): ("Bom trabalho!", "¡Buen trabajo!"),
    (2, 119): ("Missão fracassou...", "Misión fallida..."),
    (2, 120): ("Limite de tempo: %dmin", "Límite de tiempo: %dmin"),
    (2, 121): ("Tempo esgotado!", "¡Se acabó el tiempo!"),
    (2, 122): ("Itens de suprimento entregues.", "Objetos de suministro entregados."),
    (2, 123): ("Entregas possíveis por 20 segs!", "Entregas posibles por 20 seg."),
    (2, 124): ("Objetivo cumprido!", "¡Objetivo cumplido!"),
    (2, 125): ("Vitória!", "¡Victoria!"),
    (2, 136): ("Entrando no confronto final.", "Entrando al enfrentamiento final."),
    (2, 137): ("Dragonator pronto para usar.", "Dragonator listo para usar."),
    (2, 138): ("Gongo de caça pronto para usar.", "Gong de caza listo para usar."),
    (2, 139): ("Balista pronta para usar.", "Balista lista para usar."),
    # --- status / box ----------------------------------------------------
    (2, 509): ("Enviar ao baú", "Enviar a caja"),
    (2, 543): ("Bolsa", "Bolsa"),
    (2, 545): ("Bolsa Missão", "Bolsa Mis."),
    (2, 550): ("Status", "Estado"),
    (2, 551): ("Equip.", "Equipo"),
    (2, 552): ("Nome", "Nombre"),
    (2, 553): ("Arma", "Arma"),
    (2, 554): ("Local", "Lugar"),
    (2, 555): ("Dinheiro", "Dinero"),
    (2, 556): ("Pts Yukumo", "Pts Yukumo"),
    (2, 557): ("Pts Guilda", "Pts Gremio"),
    (2, 559): ("Vida", "Vida"),
    (2, 560): ("Vigor", "Aguante"),
    (2, 561): ("Ataque", "Ataque"),
    (2, 562): ("Defesa", "Defensa"),
    (2, 563): ("Res. Fogo", "Res. Fuego"),
    (2, 564): ("Res. Água", "Res. Agua"),
    (2, 565): ("Res. Trovão", "Res. Rayo"),
    (2, 566): ("Res. Gelo", "Res. Hielo"),
    (2, 567): ("Res. Dragão", "Res. Dragón"),
    (2, 586): ("Info. habil.", "Info. habilidad"),
    (2, 587): ("Habilidade", "Habilidad"),
    (2, 588): ("Pts", "Pts"),
    (2, 601): ("Espaços", "Espacios"),
    (2, 602): ("Afinidade", "Afinidad"),
    (2, 606): ("Defesa", "Defensa"),
    (2, 951): ("Status", "Estado"),
    (2, 1562): ("Enviar ao baú", "Enviar a caja"),
    (2, 1998): ("Status", "Estado"),
    (2, 1999): ("Equip.", "Equipo"),
    # --- weapon names ----------------------------------------------------
    (2, 569): ("Espadão", "Mandoble"),
    (2, 570): ("Espada e Escudo", "Espada y Escudo"),
    (2, 571): ("Martelo", "Martillo"),
    (2, 572): ("Lança", "Lanza"),
    (2, 573): ("B. Pesada", "B. Pesada"),
    (2, 574): ("B. Média", "B. Media"),
    (2, 575): ("B. Leve", "B. Ligera"),
    (2, 576): ("Espada Longa", "Sable Largo"),
    (2, 577): ("Machado", "Hacha"),
    (2, 578): ("Lança-Fuzil", "Lanza-Cañón"),
    (2, 579): ("Arco", "Arco"),
    (2, 580): ("Lâminas Duplas", "Hojas Dobles"),
    (2, 581): ("Trompa", "Trompa"),
    (2, 585): ("C", "C"),
    (2, 590): ("E", "E"),
}

rows_out = []
missing = []
for (table, index), (pt, es) in sorted(T.items()):
    en = english(16, table, index)
    if en is None:
        missing.append((table, index))
        continue
    rows_out.append((16, table, index, en, pt, es))

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("# Glossary for the game's own text, one row per string.\n")
    f.write("# Columns: entry, table, index, english, portuguese, spanish\n")
    f.write("# English is what the patched disc shows (the key); tools/build_lang.py\n")
    f.write("# turns the Portuguese and Spanish columns into pt-BR.lang and es.lang.\n")
    f.write("# The game's formatting codes (~Cnn colour, ~Bnn button) are kept in the text.\n")
    f.write("#\n")
    f.write("# Entries not listed here fall back to the game's own text.\n")
    for entry, table, index, en, pt, es in rows_out:
        f.write("%d\t%d\t%d\t%s\t%s\t%s\n" % (entry, table, index, en, pt, es))

print("wrote", OUT, "rows:", len(rows_out))
if missing:
    print("MISSING on disc:", missing)
