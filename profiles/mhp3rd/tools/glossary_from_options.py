# The game's own Options menu (table 2, 1377-1461) and the language/sound
# labels. Appended to the glossary.
import csv

DUMP = r"C:\trabalios\istudous\romhacking\mhp3rd\Yakumo\docs\TEXT_DUMP\strings.csv"
GLOS = r"C:\trabalios\istudous\romhacking\mhp3rd\Yakumo\profiles\mhp3rd\translations\glossary.tsv"

by_key = {}
for r in csv.DictReader(open(DUMP, encoding="utf-8")):
    by_key[(r["entry"], r["table"], r["index"])] = r["text"]

T = {
    1304: ("IDIOMA", "IDIOMA"),
    1305: ("SOM", "SONIDO"),
    1306: ("VOLUME BGM", "VOLUMEN BGM"),
    1307: ("VOLUME EFEITOS", "VOLUMEN EFECTOS"),
    1308: ("CONTINUAR RÁPIDO", "CONTINUAR RÁPIDO"),
    1309: ("EFEITOS DE TELA", "EFECTOS DE PANTALLA"),
    1310: ("EFEITOS", "EFECTOS"),
    1311: ("AJUSTES PADRÃO", "AJUSTES PREDETERMINADOS"),
    1314: ("INGLÊS", "INGLÉS"),
    1315: ("ALEMÃO", "ALEMÁN"),
    1316: ("FRANCÊS", "FRANCÉS"),
    1317: ("ESPANHOL", "ESPAÑOL"),
    1318: ("ITALIANO", "ITALIANO"),
    1319: ("ESTÉREO", "ESTÉREO"),
    1320: ("DOLBY PRO LOGIC Ⅱ", "DOLBY PRO LOGIC Ⅱ"),
    1321: ("REFORÇO", "REFUERZO"),
    1322: ("MÍN", "MÍN"),
    1323: ("MÁX", "MÁX"),
    1324: ("DESLIG.", "APAGADO"),
    1325: ("LIG.", "ENCENDIDO"),
    1326: ("NORMAL", "NORMAL"),
    1327: ("REDUZIDO", "REDUCIDO"),
    1330: ("Ajusta o volume da música.", "Ajusta el volumen de la música."),
    1340: ("Voltar ao menu do jogo.", "Volver al menú del juego."),
    1351: ("Combinar itens.", "Combinar objetos."),
    1355: ("Pausar o jogo.", "Pausar el juego."),
    1362: ("Rever os conselhos.", "Revisar los consejos."),
    1364: ("Ver sua carta de guilda.", "Ver tu carta de gremio."),
    1365: ("Editar sua carta de guilda.", "Editar tu carta de gremio."),
    1371: ("Dispensar um camarada.", "Despedir a un camarada."),
    1377: ("Opções", "Opciones"),
    1378: ("Mostrar status", "Mostrar estado"),
    1379: ("Mostrar mapa", "Mostrar mapa"),
    1380: ("Câmera inicial", "Cámara inicial"),
    1381: ("Config. câmera", "Ajustes cámara"),
    1382: ("Mira", "Mira"),
    1383: ("Controles de mira", "Controles de puntería"),
    1384: ("Tipo de câmera", "Tipo de cámara"),
    1385: ("Esquiva", "Esquiva"),
    1386: ("Tipo de mira", "Tipo de mira"),
    1387: ("Som", "Sonido"),
    1388: ("Efeitos de tela", "Efectos de pantalla"),
    1389: ("AJUSTES PADRÃO", "AJUSTES PREDETERMINADOS"),
    1394: ("NORMAL", "NORMAL"),
    1395: ("INVERT. 1", "INVERTIR 1"),
    1396: ("INVERT. 2", "INVERTIR 2"),
    1397: ("TIPO 1", "TIPO 1"),
    1398: ("TIPO 2", "TIPO 2"),
    1399: ("TIPO 3", "TIPO 3"),
    1400: ("TIPO 4", "TIPO 4"),
    1401: ("TIPO 5", "TIPO 5"),
    1402: ("ESTÉREO", "ESTÉREO"),
    1403: ("DPLⅡ", "DPLⅡ"),
    1404: ("REFORÇO", "REFUERZO"),
    1405: ("AUTO", "AUTO"),
    1406: ("MANUAL", "MANUAL"),
    1407: ("Mostrar a interface.", "Mostrar la interfaz."),
    1408: ("Não mostrar a interface.", "No mostrar la interfaz."),
    1409: ("Mostrar o mapa.", "Mostrar el mapa."),
    1410: ("Não mostrar o mapa.", "No mostrar el mapa."),
    1448: ("Item enviado.", "Objeto enviado."),
    1452: ("Descartar o item selecionado?", "¿Descartar el objeto elegido?"),
    1453: ("Selecione o caçador.", "Elige al cazador."),
    1454: ("Confirmar?", "¿Está bien?"),
    1457: ("Entregar estes itens?", "¿Entregar estos objetos?"),
    1458: ("Ordenar itens?", "¿Ordenar objetos?"),
    1461: ("Desconhecido", "Desconocido"),
}

lines = []
for index, (pt, es) in sorted(T.items()):
    en = by_key.get(("16", "2", str(index)))
    if en is None:
        continue
    lines.append("16\t2\t%d\t%s\t%s\t%s\n" % (index, en, pt, es))

with open(GLOS, "a", encoding="utf-8", newline="\n") as handle:
    handle.write("\n# --- the game's Options menu (table 2) ---\n")
    for line in lines:
        handle.write(line)
print("rows added:", len(lines))
