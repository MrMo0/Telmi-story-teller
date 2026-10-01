# Telmi OS — fork MrMo0

Fork de [DantSu/Telmi-story-teller](https://github.com/DantSu/Telmi-story-teller) (basé sur la v1.10.4).
Pour la présentation, l'installation et l'utilisation de Telmi OS, voir le dépôt d'origine et [telmi.fr](https://telmi.fr).

## Différences avec Telmi OS

| Bouton | Où | Action |
|---|---|---|
| L1 / R1 | liste des histoires ou albums, vue mosaïque | page précédente / suivante |
| L2 | liste des histoires | bascule `title.png` ↔ `cover.png` (repli sur `title.png` si pas de cover) |
| Flèche haut maintenue | à l'allumage | démarre OnionOS installé sur la 2ᵉ partition de la SD |

Menu + L2 règle toujours la luminosité.

### Dual boot Telmi / Onion

Sans option ni menu : rien pressé → Telmi, flèche haut maintenue à l'allumage → Onion.

| Partition (MBR) | Format | Contenu |
|---|---|---|
| 1 | FAT32 | Telmi OS (ce fork) + `Stories/`, `Saves/`, `Music/` |
| 2 | FAT32 | Onion OS, copié tel quel |

La 2ᵉ partition est montée sur `/mnt/SDCARD` avant de lancer Onion : il ne voit ni ne modifie la
partition Telmi, qui reste reconnue par Telmi Sync. Sans 2ᵉ partition, Telmi démarre normalement.
Pendant l'installation d'Onion, maintenir la flèche haut à chaque redémarrage.

## Build

```
docker run --rm -v "$PWD":/root/workspace aemiii91/miyoomini-toolchain:latest \
  /bin/bash -c 'source /root/.bashrc; cd /root/workspace && make dist'
```

Copier le contenu de `build/` à la racine de la 1ʳᵉ partition.

⚠️ Une mise à jour de Telmi OS par Telmi Sync installe la version officielle à la place de ce fork.
