# Changelog

Ogni versione ha una sezione `## x.y.z`: il testo viene usato così com'è per la release su GitHub,
per il messaggio del commit e per la finestra di aggiornamento dentro l'app.

## 0.5.6

- Proxy per gli stream camuffati (server HD-1 di Anichi/Anikoto) riscritto: usa pochi thread fissi invece di crearne uno per ogni segmento, niente più chiusure dell'app
- I sottotitoli usano sempre i font della console (tolta l'opzione: senza, non si vedevano)
- Se l'app si chiude durante un video, al riavvio spegne il proxy e lo spiega; si riattiva in Impostazioni > Riproduzione
- Corretto un errore per cui il proxy si attivava su tutti gli stream HLS: AnimeSaturn (es. One Piece 1178) restava su "Caricamento"
- Diario del proxy in sdmc:/switch/AnikkuNX/proxy.log

## 0.5.5

- Corretto il crash all'apertura dei video con sottotitoli: il player carica solo il font della console che serve
- Se l'app si chiude durante la riproduzione, al riavvio disattiva da sola i font dei sottotitoli (e poi il proxy degli stream) e lo spiega; si riattivano in Impostazioni > Riproduzione

## 0.5.4

- Nuovo pulsante nel player per la velocità di riproduzione: 1x, 1.5x, 2x (anche con ZR)
- Sottotitoli finalmente visibili: il player usa i font della console. Quelli esterni si attivano da soli, nella lingua della console o in inglese
- Corretti i video che saltavano subito alla fine (server HD-1 di Anichi/Anikoto e simili): i segmenti camuffati da immagine ora vengono ripuliti
- Se uno stream si interrompe a metà, l'episodio non viene segnato come visto e si prova il server successivo
- Nuove fonti in altre lingue (da attivare in Impostazioni → Scegli le fonti), tutte provate fino al video:
  - Spagnolo: Jkanime, AnimeAV1, Latanime, MundoDonghua, VerAni.me, VerAnimes, BeatZ Anime, PelisPlus, Cineplus123, VerPelisTop
  - Portoghese: Animes Digital · Francese: Anime-Sama, Vostfree · Tedesco: AniWorld, AnimeToast
  - Arabo: Asia2TV, TukTukCinema · Russo: Animevost, YummyAnime · Indonesiano: Samehadaku
  - Cinese: Xifan, Aiyifan, Nivod, Xiaobao · Serbo: AnimeBalkan

## 0.5.3

- Fonti inglesi verificate a fondo, fino al primo segmento video
- Corretto ok.ru (AnimeKhor, AnimeXin, DonghuaStream): i video ora partono
- Rumble aggiornato; i link Dailymotion bloccati vengono saltati
- Tolte AnimePahe e ChineseAnime: i siti richiedono una verifica Cloudflare fattibile solo da browser

## 0.5.2

- Avviso in alto a destra se l'app è aperta in modalità applet (dall'Album): memoria limitata, i video possono bloccarsi
- Come avere la memoria piena: tieni premuto R mentre avvii un gioco qualsiasi, poi apri AnikkuNX dal menu homebrew
- Oppure crea un forwarder con Sphaira (X su AnikkuNX > Install Forwarder) e avvia AnikkuNX dalla Home
- EN: for full memory, hold R while launching any game and open AnikkuNX from the homebrew menu, or install a Sphaira forwarder

## 0.5.1

- Corretta la finestra di aggiornamento: le note lunghe ora scorrono e restano nello schermo

## 0.5.0

- Nuovi episodi della libreria: all'avvio, appena la console è connessa a Internet, l'app controlla gli anime in libreria
- Badge "+N" sulle copertine degli anime con episodi nuovi, che salgono in cima alla Libreria
- Notifica quando escono nuovi episodi; il badge si azzera aprendo l'anime
- Nuova opzione in Impostazioni per disattivare il controllo dei nuovi episodi
- Il changelog delle versioni ora è mostrato per intero anche nella finestra di aggiornamento
- New: library new-episode check at startup, "+N" badges, notifications and a setting to turn it off

## 0.4.3

- Versione di prova per verificare l'aggiornamento in-app

## 0.4.2

- Corretto l'aggiornamento in-app ("impossibile sostituire" il .nro in esecuzione)

## 0.4.1

- Istruzioni in Impostazioni per creare un'icona nella Home (forwarder) con Sphaira

## 0.4.0

- Aggiornamento in-app dalle release di GitHub, solo quando la console è connessa a Internet
- Versione completa e "Controlla aggiornamenti" nelle Impostazioni
