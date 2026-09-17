# Plan de développement : Seek MP3 rapide avec mpg123 direct

> **Objectif de ce document** : plan complet pour une nouvelle session de développement.
> Remplacer SDL_mixer par **mpg123 direct** afin que le seek arrière (-10s) soit aussi
> rapide que le seek avant (+10s).

---

## 1. Contexte et but

### Problème
Dans le lecteur d'histoires (`stories_reader`), le **seek arrière (-10s) est lent**
(proportionnel à la position courante dans la piste). Le seek avant (+10s) est quasi
instantané. L'utilisateur perçoit un gel de l'audio à chaque retour en arrière.

### Cause racine
Le pipeline audio actuel utilise **SDL_mixer** (`Mix_Music`). `Mix_SetMusicPosition()`
sur un MP3 **re-décode depuis le début** pour un seek arrière → O(position courante).

### Solution retenue (Option B)
Piloter **mpg123 directement** (plus SDL_mixer pour le MP3). mpg123 sait chercher à un
offset de byte / un numéro de frame **sans re-décoder** → seek O(1) ou O(log n), dans les
deux sens.

### But concret
- Seek arrière aussi rapide que le seek avant.
- Pas de gel, pas de silence anormal.
- Conserver exactement le même comportement visible (timeline, pause, autoplay, mode nuit).

---

## 2. État actuel (code)

### 2.1 Pipeline audio actuel — `src/storyTeller/sdl_helper.h`

Fonctions publiques (API à conserver) :

| Fonction | Implémentation actuelle | Rôle |
|---|---|---|
| `audio_play_path(path, position, askDuration)` | `Mix_LoadMUS` + `Mix_PlayMusic` + `Mix_SetMusicPosition` | Charge + joue + seek initial |
| `audio_play(dir, name, position, askDuration)` | assemble le path puis `audio_play_path` | variante dir/name |
| `audio_setPosition(position)` | `Mix_SetMusicPosition(position)` | **seek (lent en arrière)** |
| `audio_getPosition()` | `Mix_GetMusicPosition(music)` | position courante (s) |
| `audio_getDuration()` | `musicDuration` | durée totale (s) |
| `audio_isFinished()` | `music == NULL \|\| Mix_PlayingMusic() == 0` | fin de piste ? |
| `audio_free_music()` | `Mix_HaltMusic` + `Mix_FreeMusic` | libère la musique |
| `audio_duration_cache_get/set` | cache LRU de durées | évite de recalculer la durée |

Détail `audio_play_path` :
```c
audio_free_music();
music = Mix_LoadMUS(soundPath);          // charge le MP3 en mémoire
if (music != NULL) {
    musicDuration = ...;                 // cache -> mp3_duration_estimate -> Mix_MusicDuration
    Mix_PlayMusic(music, 1);            // joue une fois
    Mix_SetMusicPosition(position);      // SEEK INITIAL (lent si position > 0)
}
```

Init/quit :
- `video_audio_init()` : `SDL_Init(VIDEO|AUDIO)`, `Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096)`, `Mix_Init(MIX_INIT_MP3)`, `Mix_Volume...`.
- `video_audio_quit()` : `Mix_FreeMusic`, `Mix_CloseAudio`, `SDL_Quit`.

### 2.2 Consommation — `src/storyTeller/stories_reader.h`

Appels à l'API `audio_*` :
- `stories_rewind(time)` (≈ ligne 1062) : `storyTime = audio_getPosition() + time` ; clamp à `[0, duration]` ; `audio_setPosition(storyTime)`.
- `stories_next` / `stories_previous` : `stories_rewind(+10)` / `stories_rewind(-10)` (mode autoplay ou nuit).
- `stories_drawTimeline` (≈ ligne 587) : `audio_getPosition()`, `audio_getDuration()`, **`Mix_PausedMusic() == 1`** (icône pause/play).
- `stories_load` (≈ ligne 718) : `audio_play(story_audio_path, name, storyTime, !isImageDefined)`.
- `stories_title` (≈ ligne 985) : `audio_play(story_path, "title.mp3", storyTime, false)`.
- `stories_home` / `stories_reset` : `audio_free_music()`.
- `stories_callCallback` (≈ ligne 1251) : `if (callback_stories_audio_hook != NULL && audio_isFinished())` → hook de fin.

**⚠️ Point critique** : le code appelle **directement** les fonctions SDL_mixer (pas via
les wrappers `audio_*`) :
- `Mix_PlayingMusic()` (stories_pause, stories_save)
- `Mix_PausedMusic()` (stories_pause, stories_save, stories_drawTimeline)
- `Mix_ResumeMusic()` (stories_pause)
- `Mix_PauseMusic()` (stories_pause)

→ Ces appels **doivent** être remplacés par des wrappers `audio_*` pour découpler le code
du moteur audio (SDL_mixer → mpg123).

### 2.3 Consommation — `src/storyTeller/music_player.h` (lecteur MP3)

Le lecteur MP3 utilise **la même API `audio_*`** ET **les mêmes `Mix_*` directs** :
- `audio_getPosition`, `audio_getDuration`, `audio_isFinished`, `audio_play`,
  `audio_setPosition`, `audio_free_music` (mêmes que stories_reader).
- `Mix_PlayingMusic()` (lignes 293, 446, 463, 501)
- `Mix_PausedMusic()` (lignes 293, 447, 463, 502)
- `Mix_PauseMusic()` (lignes 298, 451)
- `Mix_ResumeMusic()` (ligne 448)

→ **Les deux fichiers** (`stories_reader.h` ET `music_player.h`) doivent remplacer leurs
`Mix_*` directs par les wrappers `audio_*` (§4.3). SDL_mixer est donc utilisé par les
**deux** apps — voir Q4.

### 2.4 Estimation de durée — `src/storyTeller/mp3_helper.h`
- `mp3_duration_estimate(path)` : estimation rapide de la durée d'un MP3 **CBR** (lit le
  1er frame pour le bitrate, déduit les tags ID3, détecte le VBR). Retourne -1 si VBR ou
  pas de frame valide → le caller replie sur `Mix_MusicDuration`.
- `mp3_frame_bitrate(data, length)` : bitrate du 1er frame MP3 Layer III.

→ **Ce module sera renommé et agrandi** (voir §3.0) : tout le code MP3 (lecture, durée,
position, seek) ira dedans.

---

## 3. Architecture cible

Remplacer SDL_mixer (pour le MP3) par **mpg123 + un device audio SDL en streaming**.

### 3.0 Réorganisation des modules (renommage `mp3_helper.h`)

`mp3_helper.h` n'a plus lieu d'être en tant que simple « helper ». Il est **renommé** en
**`mp3_reader.h`** et devient **le module unique de lecture MP3** :

- **Nom recommandé : `mp3_reader.h`** (éviter `mp3_player.h`, qui prêterait à confusion
  avec `music_player.h`, l'APP lecteur de musique).
- **Contenu** : TOUT le code associé à la lecture MP3
  - le moteur mpg123 (handle, device SDL, callback, mutex) ;
  - l'API publique `audio_*` (play, setPosition, getPosition, getDuration, isFinished,
    free, pause, resume, isPlaying, isPaused) — **déplacée depuis `sdl_helper.h`** ;
  - le calcul de durée (réutilise/étend `mp3_duration_estimate` + `mp3_frame_bitrate`) ;
  - le changement de position / seek ;
  - la gestion de fin de piste.
- **`sdl_helper.h`** ne garde que le **video/display** (+ `SDL_Init`). La partie audio
  (device SDL, callback, `audio_*`) part dans `mp3_reader.h`.

Conséquence : `stories_reader.h` et `music_player.h` appellent toujours `audio_*`, mais
ces fonctions vivent désormais dans `mp3_reader.h` (inclus par `sdl_helper.h` ou
directement par les consumers).

### 3.1 Composants (état static dans `mp3_reader.h`, comme le reste du code)

```c
static mpg123_handle *mpg = NULL;      // handle mpg123
static int  mpgFd = -1;                // fd du fichier ouvert (pour mpg123_open_fd)
static SDL_AudioDeviceID audioDevice = 0;  // device SDL (playback)
static SDL_AudioSpec have = {0};       // format réel négocié par SDL
static pthread_mutex_t audioMutex = PTHREAD_MUTEX_INITIALIZER;
static char  currentPath[STR_MAX * 2] = {0};
static double musicDuration = -1.0;
static double currentPosition = 0.0;   // position courante (s), mise à jour par le callback
static int    currentSampleRate = 44100;
static int    currentChannels = 2;
static bool   isPlaying = false;
static bool   isPaused  = false;
static bool   isFinished = false;
// demande de seek (main thread -> callback audio)
static bool   seekRequested = false;
static double seekTarget = 0.0;
```

### 3.2 Format de sortie mpg123
- Forcer le format de sortie mpg123 à **44100 Hz, S16LE, 2 canaux** via `mpg123_format`
  (ou laisser mpg123 négocier et adapter le `SDL_AudioSpec` en conséquence).
- Le device SDL est ouvert avec le même format (ou `SDL_AUDIO_ALLOW_ANY_CHANGE` +
  conversion gérée par SDL).

### 3.3 Flux de lecture (mixer callback SDL)

Le **callback audio SDL** (exécuté sur le thread audio) est le cœur :

```
audioCallback(userdata, stream, len):
    lock(audioMutex)
    if (seekRequested):
        mpg123_seek_frame(mpg, frameNumberFor(seekTarget), SEEK_SET)   // SEEK RAPIDE
        currentPosition = seekTarget
        seekRequested = false
    if (isPaused || isFinished || mpg == NULL):
        memset(stream, 0, len)                 // silence
        unlock; return
    n = mpg123_decode(mpg, in, inSize, stream, len, &frames)   // décode MP3 -> PCM
    if (n <= 0 || err == MPG123_OK but no data):
        isFinished = true
        memset(stream, 0, len)
    else:
        currentPosition += (double)n / (currentSampleRate * currentChannels * 2)
    unlock
```

- `mpg123_decode` lit le MP3 (via le fd) et produit du PCM S16LE stéréo.
- SDL consomme le PCM et le joue au bon rythme → **pas de gestion de file à faire**.
- Le seek est fait **sur le thread audio** (pas de conflit avec le main thread).

### 3.4 Seek rapide (le cœur du fix)

`audio_setPosition(position)` (main thread) :
```c
lock(audioMutex)
seekTarget = clamp(position, 0, musicDuration)
seekRequested = true
unlock
```

Le callback (thread audio) exécute réellement le seek :
```c
// frameNumber = time * sampleRate / samplesPerFrame
// samplesPerFrame = 1152 (MPEG1/2 Layer III) ou 576 (MPEG2/2.5 Layer III)
long frame = (long)(seekTarget * currentSampleRate / samplesPerFrame);
mpg123_seek_frame(mpg, frame, SEEK_SET);   // mpg123 va au frame (offset byte interne)
```

- **Pourquoi c'est rapide** : `mpg123_seek_frame`/`mpg123_seek` déplacent le pointeur de
  fichier (offset byte) puis trouvent le frame suivant. **Pas de re-décodage depuis le
  début.** → O(1) en pratique.
- **Précision** : exact pour CBR ; approximatif pour VBR (acceptable pour un seek ±10s).
  Voir §9 (Q1/Q3).

### 3.5 Pause / Resume
- `audio_pause()` : `isPaused = true` (le callback met du silence, mpg123 n'avance pas).
- `audio_resume()` : `isPaused = false` (le callback reprend le decode).
- `audio_isPlaying()` : `isPlaying && !isFinished`.
- `audio_isPaused()` : `isPaused`.

### 3.6 Fin de piste
- Quand `mpg123_decode` ne produit plus de données (fin du fichier) → `isFinished = true`.
- `audio_isFinished()` renvoie `isFinished`.
- `stories_callCallback` appelle `callback_stories_audio_hook` quand `audio_isFinished()`
  (comportement inchangé).

---

## 4. API publique (contrat à respecter)

### 4.1 Fonctions existantes — signatures inchangées
`audio_play_path`, `audio_play`, `audio_setPosition`, `audio_getPosition`,
`audio_getDuration`, `audio_isFinished`, `audio_free_music`,
`audio_duration_cache_get`, `audio_duration_cache_set`.

### 4.2 Nouvelles fonctions à ajouter (wrappers qui remplacent les `Mix_*` directs)

| Nouvelle fonction | Remplace | Implémentation |
|---|---|---|
| `bool audio_isPlaying(void)` | `Mix_PlayingMusic() == 1` | `return isPlaying && !isFinished;` |
| `bool audio_isPaused(void)` | `Mix_PausedMusic() == 1` | `return isPaused;` |
| `void audio_resume(void)` | `Mix_ResumeMusic()` | `isPaused = false;` |
| `void audio_pause(void)` | `Mix_PauseMusic()` | `isPaused = true;` |

### 4.3 Remplacements dans `stories_reader.h` ET `music_player.h`
- `Mix_PlayingMusic() == 1` → `audio_isPlaying()`
- `Mix_PausedMusic() == 1` → `audio_isPaused()`
- `Mix_ResumeMusic()` → `audio_resume()`
- `Mix_PauseMusic()` → `audio_pause()`

Fonctions concernées :
- `stories_reader.h` : `stories_pause`, `stories_save`, `stories_drawTimeline`.
- `music_player.h` : les fonctions de pause/play/avance (≈ lignes 293, 298, 446-451,
  463, 501-502).

---

## 5. Étapes d'implémentation (ordre recommandé)

### Étape 0 — Vérifier les prérequis
- [ ] `lib/libmpg123.so.0` = la version **v1.22.4-1** (celle qui marche sur la Miyoo).
- [ ] `include/mpg123/mpg123.h` = le header **accordé** à cette version (+ `fmt123.h` si inclus).
- [ ] Vérifier que le header déclare bien : `mpg123_new`, `mpg123_open_fd`,
  `mpg123_format`, `mpg123_info`, `mpg123_seek`, `mpg123_seek_frame`, `mpg123_decode`,
  `mpg123_close`, `mpg123_delete`. (Ces fonctions existent dans v1.22.4.)
- [ ] Le Makefile racine copie déjà `lib/libmpg123.so.0` dans le toolchain (à confirmer).

### Étape 1 — Renommer `mp3_helper.h` → `mp3_reader.h` + découpler de SDL_mixer
1. **Renommer** `src/storyTeller/mp3_helper.h` en `src/storyTeller/mp3_reader.h`.
2. **Déplacer** l'API `audio_*` (fonctions + état `musicDuration` + cache de durées)
   de `sdl_helper.h` vers `mp3_reader.h`. `sdl_helper.h` ne garde que video/display.
3. Ajouter dans `mp3_reader.h` : `audio_isPlaying`, `audio_isPaused`, `audio_resume`,
   `audio_pause` (implémentés via SDL_mixer pour l'instant, pour ne rien casser).
4. Dans `stories_reader.h` **et** `music_player.h` : remplacer les `Mix_*` directs par ces
   wrappers.
5. **Compiler** (l'utilisateur) → vérifier que rien ne casse. Le comportement est identique.

### Étape 2 — Implémenter le moteur mpg123 (derrière la même API, dans `mp3_reader.h`)
1. Dans `mp3_reader.h` :
   - Ajouter l'état mpg123 (§3.1) + `#include "mpg123/mpg123.h"`.
   - Réécrire `audio_play_path` : ouvrir le fd, `mpg123_new`, `mpg123_open_fd`,
     `mpg123_format` (44100/S16/2ch), `mpg123_info` (sample rate, channels), seek initial
     (`mpg123_seek_frame`), ouvrir le device SDL (1 seule fois, à l'init), `isPlaying=true`.
   - Réécrire `audio_setPosition` : set `seekRequested` + `seekTarget` (§3.4).
   - Réécrire `audio_getPosition` / `audio_getDuration` / `audio_isFinished` /
     `audio_free_music`.
   - Implémenter le **mixer callback** SDL (§3.3).
   - Implémenter `audio_pause` / `audio_resume` / `audio_isPlaying` / `audio_isPaused`.
2. Gérer la durée : réutiliser `mp3_duration_estimate` (CBR) ; si -1, utiliser
   `mpg123_info` / estimation. Garder le cache `audio_duration_cache_*`.

### Étape 3 — Init / Quit
- `video_audio_init` (`sdl_helper.h`) : `SDL_Init(VIDEO|AUDIO)`, `IMG_Init`, `TTF_Init`
  (non audio). **Ne plus** appeler `Mix_OpenAudio`/`Mix_Init`/`Mix_Volume`. Appeler
  `mp3_reader_init()` (nouvelle, dans `mp3_reader.h`) qui ouvre le device SDL audio
  **une fois** (`SDL_OpenAudioDevice` avec le callback).
- `video_audio_quit` (`sdl_helper.h`) : appeler `mp3_reader_quit()` (nouvelle, dans
  `mp3_reader.h`) qui fait `SDL_CloseAudioDevice`, `mpg123_close`/`mpg123_delete`,
  `close(mpgFd)`. Puis `SDL_Quit`. **Ne plus** appeler `Mix_CloseAudio`.
- **Question** : SDL_mixer est-il encore utilisé ailleurs (OGG, WAV, musique du menu) ?
  Voir §9 (Q4). Préférer : ne plus utiliser SDL_mixer du tout.

### Étape 4 — Makefile
- Ajouter `-lmpg123` aux `LDFLAGS` (actuellement
  `-lpthread -lSDL2 -lSDL2_image -lSDL2_ttf -lSDL2_mixer -lSDL2_gfx`).
- Le `.so.0` est déjà copié dans le toolchain par le Makefile existant (à confirmer).
- Si SDL_mixer n'est plus du tout utilisé, on pourra retirer `-lSDL2_mixer` (optionnel,
  pas prioritaire).

### Étape 5 — Tests (voir §8)

---

## 6. Règles à respecter (contraintes projet)

- **NE JAMAIS compiler** : l'utilisateur compile lui-même. Ne jamais lancer
  `make`/`gcc`/`g++`/outils de compilation.
- **C18** (`-std=gnu18`).
- **Header-only** : chaque module storyTeller est un header auto-contenu
  (`static` + `#ifndef` guards). Pas de séparation .h/.c.
- `STR_MAX` = taille de buffer standard (défini dans `utils/str.h`).
- `STR_DIRNAME` = 128.
- `DISPLAY_WIDTH` = 640, `DISPLAY_HEIGHT` = 480.
- **Conserver l'API publique `audio_*`** (mêmes signatures) → le reste du code ne change
  que pour les `Mix_*` directs (§4.3).
- **Pas de fuite de fd / de handle** : chaque `open`/`mpg123_new` a un point de sortie
  unique de fermeture (convention existante du code, cf. `mp3_reader.h`).
- **Thread-safety** : tout accès à l'état mpg123 / à `currentPosition` passe par
  `audioMutex`. Le main thread ne touche **jamais** au handle mpg123 directement
  (il set seulement `seekRequested`/`seekTarget` sous mutex).
- NE PAS aller dans `/design/`. NE PAS chercher de code dans `/build`, `/cache`, `/release`.

---

## 7. Risques et mitigations

| Risque | Impact | Mitigation |
|---|---|---|
| File I/O sur le thread audio (SD lente) → glitch | Audio qui saccade | Lire le MP3 en chunks plus gros ; ou charger le fichier en mémoire et `mpg123_open_feed`. Mesurer en premier. |
| VBR : seek imprecis | Seek qui tombe à côté (±quelques s) | `mpg123_seek_frame` (frame-based). Acceptable pour ±10s. Si insuffisant, construire un seek index (voir Q3). |
| Concurrency (main vs thread audio) | Data race, crash | Mutex unique ; le main thread ne fait que set `seekRequested`/`seekTarget`. |
| Pause/resume qui décale la position | Position qui saute | Ne pas avancer `currentPosition` quand `isPaused` ; le seek reprend de `seekTarget`. |
| Fin de piste non détectée | Hook jamais appelé | `isFinished=true` quand `mpg123_decode` ne produit plus de données. |
| Format audio non aligné (rate/channels) | Distorsion, vitesse fausse | `mpg123_format` → adapter `SDL_AudioSpec` ; vérifier `mpg123_info`. |
| 2 moteurs audio (SDL_mixer + mpg123) ouverts | Conflit device | Ne garder qu'un seul moteur (mpg123). Retirer `Mix_OpenAudio` si SDL_mixer n'est plus utilisé. |
| Version mpg123 / header mismatch | Link/runtime error | Vérifier §5 Étape 0 (fonctions présentes dans le header ET le .so.0). |

---

## 8. Plan de test

### Tests fonctionnels (manuel, sur la Miyoo)
1. **Seek avant** : +10s → position correcte, **rapide**.
2. **Seek arrière** : -10s → position correcte, **rapide** (LE but du fix).
3. **Pause / Resume** : pause → silence + icône pause ; resume → reprend au même point.
4. **Fin de piste** : laisser jouer jusqu'à la fin → hook appelé (transition / next).
5. **Changement de piste** : nouvelle piste → position 0, pas de son fantôme.
6. **Timeline** : barre de progression qui avance en temps réel, icône play/pause correcte.
7. **Mode nuit** : playlist qui enchaîne les pistes (autoplay).
8. **VBR** : tester avec un MP3 VBR si disponible (précision du seek).

### Tests unitaires (optionnels, si framework GUT/GdUnit dispo)
- `mp3_duration_estimate` sur un CBR connu.
- Calcul `frameNumberFor(time)` (time → frame) pour un sample rate / layer connus.
- Clamp de `audio_setPosition` à `[0, duration]`.

### Critère d'acceptation
- Seek arrière -10s **sans gel perceptible** (comparable au seek avant).
- Aucun comportement régressif (pause, fin, nuit, timeline).

---

## 9. Décisions clés et questions ouvertes

- **Q1 — CBR ou VBR ?** Les MP3 des histoires (export Telmi Sync) sont-ils CBR ou VBR ?
  Cela détermine la précision du seek. → **À vérifier** avec un MP3 réel
  (`mpg123 --info` ou `mp3_duration_estimate` qui retourne -1 si VBR).
- **Q2 — I/O ou mémoire ?** `mpg123_open_fd` (I/O pendant lecture, simple) vs charger le
  fichier en mémoire + `mpg123_open_feed` (pas d'I/O, plus de mémoire). → Commencer par
  `mpg123_open_fd`, mesurer les glitches, basculer si besoin.
- **Q3 — `mpg123_seek` (byte) ou `mpg123_seek_frame` (frame) ?**
  - `mpg123_seek_frame` : simple, exact en CBR, approx en VBR. **Recommandé en 1er.**
  - `mpg123_seek` (byte offset) : nécessite de calculer l'offset (bitrate × time / 8),
    plus précis en CBR, compliqué en VBR.
  - Fallback avancé VBR : construire un seek index en scannant le fichier une fois à
    l'ouverture (O(taille) une fois, puis seek O(1)).
- **Q4 — SDL_mixer encore utile ?** **Confirmé** : SDL_mixer est utilisé par les **deux**
  apps (`stories_reader.h` ET `music_player.h`), via la même API `audio_*` + les `Mix_*`
  directs. → Le moteur mpg123 remplacera SDL_mixer pour les **deux**. Vérifier qu'aucun
  autre format (OGG/WAV) n'est attendu ; si non, retirer `Mix_OpenAudio`/`-lSDL2_mixer`
  complètement (ne pas ouvrir 2 moteurs audio).
- **Q5 — Version mpg123** : confirmer que le `.so.0` (v1.22.4-1) et le `.h` sont bien
  appariés et copiés dans `/lib` et `/include` (Étape 0).

---

## 10. Références (fichiers)

- `src/storyTeller/sdl_helper.h` — pipeline audio actuel ; la partie audio (API `audio_*`,
  device, callback) **part dans `mp3_reader.h`** (§3.0). Ne garde que video/display.
- `src/storyTeller/stories_reader.h` — consommation audio + `Mix_*` à remplacer.
- `src/storyTeller/mp3_reader.h` — **nouveau** (renommé depuis `mp3_helper.h`) : module
  unique de lecture MP3 (moteur mpg123 + API `audio_*` + durée + seek).
- `src/storyTeller/music_player.h` — consommateur (lecteur MP3), `Mix_*` à remplacer.
- `include/mpg123/mpg123.h` (+ `fmt123.h`) — header mpg123 (v1.22.4).
- `lib/libmpg123.so.0` — lib mpg123 (v1.22.4-1, ARM 32-bit).
- `Makefile` (racine) — copie du `.so.0` dans le toolchain + `LDFLAGS` (`-lmpg123` à ajouter).

### Fonctions mpg123 à utiliser (présentes en v1.22.4)
`mpg123_new`, `mpg123_open_fd`, `mpg123_format`, `mpg123_info`, `mpg123_seek`,
`mpg123_seek_frame`, `mpg123_decode`, `mpg123_close`, `mpg123_delete`,
`mpg123_strerror`, `mpg123_tell`.

### Fonctions SDL audio à utiliser
`SDL_OpenAudioDevice`, `SDL_PauseAudioDevice`, `SDL_CloseAudioDevice`,
`SDL_QueueAudio`/`SDL_ClearAudio` (si approche par file), `SDL_AudioSpec`.
