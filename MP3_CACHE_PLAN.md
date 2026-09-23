# Plan : Cache de scan VBR pour mpg123

## Objectif

Mettre en cache le résultat du scan VBR (durée + index de seek) dans un fichier,
pour éviter le re-scan (lent) à chaque ouverture d'un VBR. Le cache est invalidé
si le fichier source a été modifié (détection par taille du fichier en octets).

Ce plan est autonome : tout ce qu'il faut pour développer la feature est décrit ici.

---

## 1. Contexte (état actuel du code)

Fichier principal : `src/storyTeller/mp3_reader.h` (header auto-contenu, tout est `static`).

### 1.1 Fonctions existantes pertinentes

| Fonction | Rôle |
|---|---|
| `string_hash(const char *path)` | Hash FNV-1a 64 bits (utilisé par le cache mémoire de durée). **À ne pas réutiliser** pour le cache fichier (on veut du MD5). |
| `mp3_duration_estimate(const char *path)` | Estime la durée. Renvoie `-1.0` si le fichier est **VBR** (détection : bitrate du 1er frame ≠ bitrate d'un frame au milieu). Renvoie la durée si **CBR**. |
| `audio_duration_cache_get/set` | Cache **mémoire** de durée (tableau statique de 128 entrées, hash FNV-1a). Volatile (perdu au reboot). **On le garde**, c'est le fast path. |
| `audio_play_path(char *soundPath, double position, bool askDuration)` | Ouvre le fichier, crée le handle mpg123, calcule la durée, fait le seek initial. **C'est le point d'intégration.** |
| `mp3_frameForTime`, `mp3_frame_bitrate` | Helpers existants (conversion temps→frame, parse de bitrate). |

### 1.2 Flux actuel dans `audio_play_path` (bloc `askDuration`)

```c
if (askDuration) {
    double cachedDuration = audio_duration_cache_get(soundPath);
    if (cachedDuration >= 0.0) {
        musicDuration = cachedDuration;                 // cache mémoire hit
    } else {
        musicDuration = mp3_duration_estimate(soundPath);
        if (musicDuration < 0.0) {                      // → VBR
            if (mpg123_scan(handle) == MPG123_OK) {     // scan complet (lent)
                off_t length = mpg123_length(handle);
                if (length > 0) {
                    double sourceRate = (double) MP3_AUDIO_SAMPLE_RATE;
                    struct mpg123_frameinfo frameInfo;
                    if (mpg123_info(handle, &frameInfo) == MPG123_OK && frameInfo.rate > 0) {
                        sourceRate = (double) frameInfo.rate;
                    }
                    musicDuration = (double) length / sourceRate;
                }
            }
        }
        audio_duration_cache_set(soundPath, musicDuration);
    }
}
```

### 1.3 Helpers fichiers dispo (`src/common/utils/file.h`)

| Helper | Rôle |
|---|---|
| `mkdirs(const char *dir_path)` | Crée les dossiers (équivalent `mkdir -p`). |
| `exists(const char *path)` | Vrai si le chemin existe. |
| `is_file`, `is_dir` | Tests de type. |
| `file_read(const char *path)` | Lit un fichier (texte). |

Pour le binaire : `fopen/fread/fwrite/fseek/fflush/fsync` (standard C) + `stat()` (déjà inclus via `<sys/stat.h>`).

### 1.4 MD5 : à implémenter

**SDL2_test n'est PAS linké** (pas de `libSDL2_test.so` dans `lib/`, pas de `-lSDL2_test` dans le Makefile).
Le header `include/SDL2/SDL_test_md5.h` existe mais son implémentation (`SDL_test_md5.c`) n'est pas dispo.
→ **Il faut implémenter un MD5 simple** (~100 lignes de C, algorithme standard).

---

## 2. Décisions déjà prises (ne pas rediscuter)

| Décision | Valeur |
|---|---|
| Hash du path | **MD5** (32 caractères hex), pas `string_hash`. |
| Périmètre du cache | **Tous les VBR** (rare sur la Telmi, ça ne fera pas grand chose). |
| Détection de modification | **Taille du fichier en octets** (`stat().st_size`). **Pas de mtime.** |
| Cache mémoire de durée | **On le garde** (fast path, volatile). Le cache fichier est en dessous. |
| CBR | **Aucun changement** (pas de scan, pas de cache fichier). |

---

## 3. Format du fichier de cache

### 3.1 Emplacement et nom

```
/mnt/SDCARD/.cache/mp3scan/<MD5>.cache
```

- `<MD5>` = MD5 du **path complet** du mp3 (ex. `/mnt/SDCARD/Music/Artiste_Album_Track_Titre.mp3`), en 32 caractères hex minuscules.
- Le dossier `/mnt/SDCARD/.cache/mp3scan/` est créé avec `mkdirs()` s'il n'existe pas.

### 3.2 Layout binaire (ordre des champs, little-endian)

**IMPORTANT** : le layout est une **séquence de blocs** écrits/lus avec `fwrite/fread` de blocs.
Les offsets exacts dépendent de `sizeof(off_t)`, `sizeof(size_t)` et de l'alignement sur la cible.
Miyoo Mini Plus = ARM Cortex-A7 **32 bits** → `off_t` = 4 octets, `size_t` = 4 octets, `double` = 8 octets.
**Ne pas coder les offsets en dur.** Vérifier `sizeof(off_t)` et `sizeof(size_t)` au dev.

```
Ordre  Type       Champ
─────  ─────────  ──────────────────────────────────────
1      uint32_t   magic   = 0x43333350 (bytes "MP3C" en LE)
2      int        version = 1
3      off_t      fileSize (taille du mp3 au moment du scan)
4      double     duration (durée en secondes)
5      off_t      step     (pas de l'index mpg123)
6      size_t     fill     (nombre d'entries de l'index)
7      off_t[]    offsets[fill] (les offsets de l'index mpg123)
```

- **Taille du header** : `sizeof(uint32_t)+sizeof(int)+sizeof(off_t)+sizeof(double)+sizeof(off_t)+sizeof(size_t)` (≈ 28 octets sur ARM 32 bits).
- **Taille totale** : header + `fill * sizeof(off_t)`.
- **Pour un morceau de 5 min** : `fill` ≈ 11 500 (MPEG1) à 22 900 (MPEG2) → fichier de cache ≈ **45 Ko à 90 Ko** (sur ARM 32 bits).

### 3.3 Constantes

```c
#define MP3_CACHE_DIR        "/mnt/SDCARD/.cache/mp3scan"
#define MP3_CACHE_MAGIC      0x43333350u   // "MP3C"
#define MP3_CACHE_VERSION    1
// Calculé avec sizeof (pas codé en dur) : adapte à la cible
#define MP3_CACHE_HEADER_SIZE (sizeof(uint32_t) + sizeof(int) + sizeof(off_t) + sizeof(double) + sizeof(off_t) + sizeof(size_t))
#define MP3_CACHE_MAX_FILL   200000        // garde anti-malloc-énorme si header corrompu
```

---

## 4. Logique d'invalidation

À chaque ouverture d'un **VBR** (c.-à-d. quand `mp3_duration_estimate` renvoie `-1.0`) :

```
1. stat() le mp3 → fileSize (off_t)
   - si stat() échoue ou fileSize <= 0 → pas de cache (scan normal)
2. md5 = MD5(path)
   cachePath = MP3_CACHE_DIR "/" md5 ".cache"
3. exists(cachePath) ?
   - Non  → scan complet → durée + index → mp3_scanCache_set()
   - Oui  → lire le header (40 octets)
     - magic == MP3_CACHE_MAGIC ?
     - version == MP3_CACHE_VERSION ?
     - header.fileSize == fileSize ?
     - fill > 0 ?
       - Tout OK → lire offsets[fill] → mpg123_set_index() → durée en mémoire → PAS de scan ✅
       - Sinon   → scan complet → durée + index → mp3_scanCache_set() (écrase l'ancien)
```

**Règle d'or** : si `fileSize` (actuel) ≠ `header.fileSize` (cache) → **cache invalide → re-scan**.
Pas de mtime, pas de hash du contenu. Juste la taille.

---

## 5. Fonctions à implémenter (dans `mp3_reader.h`)

Toutes en `static`, dans `mp3_reader.h`, après `mp3_duration_estimate` (ou avant `audio_play_path`).

### 5.1 MD5 (nouveau)

```c
// MD5 standard (RFC 1321). ~100 lignes.
// md5_string(path, out) : out doit faire >= 33 octets (32 hex + '\0').
static void md5_string(const char *str, char *out);
```

Implémentation standard (init/update/final). On n'a besoin que de hasher de courtes chaînes
(les paths), pas de gros flux. Une implémentation compacte suffit.

### 5.2 `mp3_scanCache_get` (nouveau)

```c
// Renvoie la durée du cache si valide, sinon -1.0.
// Si valide : restaure l'index de seek dans `handle` via mpg123_set_index().
static double mp3_scanCache_get(const char *path, mpg123_handle *handle);
```

Implémentation :
1. `stat(path)` → `fileSize`. Si échec ou `fileSize <= 0` → `-1.0`.
2. `md5_string(path, md5)` ; `snprintf(cachePath, ..., MP3_CACHE_DIR "/%s.cache", md5)`.
3. `exists(cachePath)` ? Si non → `-1.0`.
4. `fopen(cachePath, "rb")`. Lire le header (40 octets) :
   - `magic` (4), `version` (4), `fileSize` (8), `duration` (8), `step` (8), `fill` (8).
   - Si lecture partielle → `fclose` → `-1.0`.
5. Valider : `magic == MP3_CACHE_MAGIC && version == MP3_CACHE_VERSION && fileSize == headerFileSize && fill > 0 && fill < MP3_CACHE_MAX_FILL`.
   - Si invalide → `fclose` → `-1.0`.
6. `offsets = malloc(fill * sizeof(off_t))`. `fseek(fp, MP3_CACHE_HEADER_SIZE, SEEK_SET)`.
   `fread(offsets, sizeof(off_t), fill, fp)`. Si lecture partielle → `free` + `fclose` → `-1.0`.
7. `mpg123_set_index(handle, offsets, step, fill)`. Si `!= MPG123_OK` → `free` + `fclose` → `-1.0`.
8. `free(offsets)` ; `fclose(fp)` ; **retourner `duration`**.

**Sécurité** : `MP3_CACHE_MAX_FILL` (ex. `#define MP3_CACHE_MAX_FILL 200000`) pour éviter un
`malloc` énorme si le header est corrompu. Si `fill > MP3_CACHE_MAX_FILL` → invalide.

### 5.3 `mp3_scanCache_set` (nouveau)

```c
// Écrit le cache (durée + index de seek) pour `path`.
// À appeler APRÈS mpg123_scan() (l'index doit être construit).
static void mp3_scanCache_set(const char *path, mpg123_handle *handle, double duration);
```

Implémentation :
1. `stat(path)` → `fileSize`. Si échec ou `fileSize <= 0` → return.
2. `md5_string(path, md5)` ; `snprintf(cachePath, ..., MP3_CACHE_DIR "/%s.cache", md5)`.
3. `mpg123_index(handle, &offsets, &step, &fill)`. Si `!= MPG123_OK` ou `fill == 0` → return.
   - **Note** : `mpg123_index` alloue `offsets` (mpg123 le gère). On doit `free(offsets)` après.
4. `mkdirs(MP3_CACHE_DIR)`.
5. **Écriture atomique** :
   - `snprintf(tempPath, ..., cachePath ".tmp")`.
   - `fopen(tempPath, "wb")`.
   - `fwrite` : `magic` (4), `version` (4), `fileSize` (8), `duration` (8), `step` (8), `fill` (8), `offsets` (fill*8).
   - `fflush` + `fsync(fileno(fp))` + `fclose`.
   - `rename(tempPath, cachePath)`.
6. `free(offsets)`.

**Note sur `mpg123_index`** : la signature est
`int mpg123_index(mpg123_handle *mh, off_t **offsets, off_t *step, size_t *fill)`.
mpg123 alloue `*offsets` (taille `*fill`). On doit `free(*offsets)` après usage.
Vérifier le comportement exact dans `include/mpg123/mpg123.h` au moment du dev.

---

## 6. Intégration dans `audio_play_path`

Remplacer le bloc `askDuration` actuel par :

```c
if (askDuration) {
    double cachedDuration = audio_duration_cache_get(soundPath);
    if (cachedDuration >= 0.0) {
        musicDuration = cachedDuration;                 // fast path mémoire
    } else {
        musicDuration = mp3_duration_estimate(soundPath);
        if (musicDuration < 0.0) {                      // → VBR
            // NOUVEAU : cache fichier
            double cached = mp3_scanCache_get(soundPath, handle);
            if (cached >= 0.0) {
                musicDuration = cached;                 // cache fichier hit → PAS de scan
            } else {
                if (mpg123_scan(handle) == MPG123_OK) {
                    off_t length = mpg123_length(handle);
                    if (length > 0) {
                        double sourceRate = (double) MP3_AUDIO_SAMPLE_RATE;
                        struct mpg123_frameinfo frameInfo;
                        if (mpg123_info(handle, &frameInfo) == MPG123_OK && frameInfo.rate > 0) {
                            sourceRate = (double) frameInfo.rate;
                        }
                        musicDuration = (double) length / sourceRate;
                    }
                    // NOUVEAU : écrire le cache fichier
                    mp3_scanCache_set(soundPath, handle, musicDuration);
                }
            }
        }
        audio_duration_cache_set(soundPath, musicDuration);
    }
}
```

**Points d'attention** :
- `mp3_scanCache_get` est appelé **avant** `mpg123_scan`. Si hit, on saute le scan.
- `mp3_scanCache_set` est appelé **après** `mpg123_scan` (l'index doit être construit).
- Si `mpg123_scan` échoue, on n'écrit pas le cache (pas d'index).
- Le cache mémoire (`audio_duration_cache_set`) est toujours mis à jour (fast path).

---

## 7. Cas limites à gérer

| Cas | Gestion |
|---|---|
| Cache corrompu (SD retirée pendant écriture) | Écriture atomique (tmp + `rename`). Validation magic + version + fill. |
| Header corrompu (fill énorme) | `fill > MP3_CACHE_MAX_FILL` → invalide. |
| `fill == 0` | Invalide (pas d'index). |
| `fileSize == 0` | Invalide (fichier vide). |
| Dossier cache inexistant | `mkdirs()` avant écriture. |
| `stat()` échec | Pas de cache (scan normal). |
| `mpg123_index` échec | Pas de cache (scan normal, pas d'écriture). |
| `mpg123_set_index` échec | Cache invalide (re-scan). |
| Path trop long pour le buffer MD5 | Le path fait max `STR_MAX` (~256). Le buffer MD5 fait 33. OK. |

---

## 8. Ce qu'il faut savoir pour développer (checklist)

### 8.1 Fichiers à modifier
- **`src/storyTeller/mp3_reader.h`** : tout le travail (MD5 + cache + intégration).

### 8.2 Fichiers à lire (pas à modifier)
- `include/mpg123/mpg123.h` : signatures de `mpg123_index`, `mpg123_set_index`, `mpg123_scan`, `mpg123_length`.
- `src/common/utils/file.h` : `mkdirs`, `exists`.
- `src/storyTeller/mp3_reader.h` : le code actuel (point d'intégration `audio_play_path`).

### 8.3 Contraintes de code
- **C18** (`-std=gnu18`).
- Tout en `static` dans le header (pas de .c séparé).
- `#ifndef` guards (déjà en place : `MP3_READER_`).
- Pas de nouvelle dépendance (pas de lib externe).
- `writeLog("mp3_reader", ...)` pour les erreurs (pas de log de succès).
- Buffers : `STR_MAX` pour les paths (défini dans `utils/str.h`).

### 8.4 Vérifications à faire au dev
1. **MD5** : tester avec un path connu (ex. `echo -n "/test" | md5sum` → comparer).
2. **Cache écriture** : ouvrir un VBR → vérifier que `/mnt/SDCARD/.cache/mp3scan/<md5>.cache` existe.
3. **Cache lecture** : rouvrir le même VBR → vérifier qu'il n'y a PAS de `mpg123_scan` (log ou timing).
4. **Invalidation** : changer la taille du mp3 (ex. `cp` un autre mp3 au même path) → vérifier que le cache est re-scanné.
5. **CBR** : vérifier qu'un CBR ne crée PAS de fichier de cache.
6. **Pas de régression** : les CBR restent rapides, les VBR restent précis.

### 8.5 À VÉRIFIER au dev (points d'incertitude)
1. **`mpg123_index` : qui alloue `offsets` ?**
   - La doc dit que mpg123 alloue `*offsets`. Vérifier dans `mpg123.h` et tester.
   - Si mpg123 alloue → on `free(offsets)` après.
   - Si mpg123 n'alloue pas → on alloue et on passe le tableau.
2. **`mpg123_set_index` : le handle doit être ouvert mais PAS encore décodé ?**
   - Vérifier si `mpg123_set_index` peut être appelé après `mpg123_open_fd` mais avant `mpg123_decode`.
   - C'est le cas dans notre flow (on appelle `mp3_scanCache_get` avant le premier `mpg123_decode`).
3. **Endian** : la plateforme est ARM little-endian. Le layout binaire est little-endian. OK.
4. **`off_t` et `size_t`** : vérifier les tailles (8 octets sur ARM 64 bits). Sur ARM 32 bits, `off_t` peut faire 4 octets. **Vérifier la taille de `off_t` sur la cible** (Miyoo Mini Plus = ARM Cortex-A7, 32 bits → `off_t` = 4 octets ?).
   - **IMPORTANT** : si `off_t` = 4 octets (32 bits), le layout change (pas de 8 octets). Adapter le layout.
   - **À VÉRIFIER** : `sizeof(off_t)` et `sizeof(size_t)` sur la cible.

### 8.6 Risques
- **`off_t` 32 bits** : si la cible est 32 bits, `off_t` = 4 octets. Le layout binaire change.
  → **Vérifier `sizeof(off_t)` sur la cible avant d'écrire le layout.**
- **`mpg123_index` / `mpg123_set_index`** : comportement exact à vérifier (allocation, état du handle).
- **Taille du cache** : ~100-200 Ko par VBR. Si l'utilisateur a 100 VBR, c'est ~10-20 Mo. Acceptable.

---

## 9. Résumé du flow final

```
Ouverture d'un mp3 (askDuration = true)
│
├─ Cache mémoire hit ? → durée en mémoire → FIN (pas de scan)
│
├─ mp3_duration_estimate
│   ├─ CBR (durée >= 0) → durée → FIN (pas de scan, pas de cache fichier)
│   └─ VBR (durée < 0)
│       ├─ mp3_scanCache_get
│       │   ├─ Hit (fileSize OK) → mpg123_set_index + durée → FIN (pas de scan) ✅
│       │   └─ Miss
│       │       ├─ mpg123_scan (lent, 1ère fois)
│       │       ├─ mpg123_length → durée
│       │       └─ mp3_scanCache_set (écrit le cache)
│       └─ audio_duration_cache_set (cache mémoire)
│
└─ Seek initial + playback
```

**Bilan** :
- **CBR (99 %)** : pas de scan, pas de cache fichier. Rapide. (inchangé)
- **VBR, 1ère ouverture** : scan complet (lent) + écriture du cache.
- **VBR, ouvertures suivantes** : lecture du cache (rapide) + `mpg123_set_index`. Pas de scan. ✅
- **VBR, fichier modifié** : re-scan (cache invalide par `fileSize`).
