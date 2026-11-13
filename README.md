# Sculpt Bass

Prototipo di strumento VST3 per macOS, versione **0.1.0 — revisione timbrica 4**. Una sola manopola trasforma un **basso naturale rotondo** in un **naturale appena più aperto**, poi in **distorto**, **synth dance** e infine **808 aggressivo**.

La revisione 4 rende Dance e 808 più percussivi nell'attacco e più ricchi di armoniche anche nel corpo della nota. I due timbri naturali e il distorto conservano il carattere della revisione 3.

## Uso

L'interfaccia riprende Sculpt Drums, con il titolo **Sculpt Bass**: grande manopola Morph, volume rotativo e indicatore VU.

Carica il plugin su una traccia MIDI e suona. Le cinque posizioni di riferimento della manopola sono:

| Morph | Carattere |
| --- | --- |
| **0%** | **Naturale rotondo:** ripresa fingerstyle mp, realmente suonata più piano, con un trattamento leggero. |
| **25%** | **Naturale più aperto:** la ripresa mp rimane prevalente, con un poco più di presenza e incisività. |
| **50%** | **Distorto:** conserva il carattere approvato della revisione precedente. |
| **75%** | **Synth dance:** attacco più percussivo e armoniche più forti ed estese anche nel corpo, con la quota di basso distorto registrato conservata. |
| **100%** | **808 aggressivo:** attacco più marcato, fondamentale corposa e un corpo più ricco di armoniche, con il gain generale mantenuto. |

Il passaggio è continuo: si possono usare tutte le sfumature fra queste posizioni. Le tre etichette **Naturale / Distorto / 808** mantengono i riferimenti principali dell'interfaccia; il tooltip di Morph descrive tutte e cinque le tappe.

Il round robin è automatico. Usa il piccolo controllo Volume per regolare l'uscita; un doppio clic riporta Morph a 0% e Volume a 0 dB.

Morph e Volume sono automatizzabili e vengono salvati nella sessione della DAW. I campioni sono incorporati nel plugin: non occorrono Kontakt, download durante l'uso o percorsi da configurare.

Nei primi due timbri la morbidezza deriva soprattutto dalla ripresa mp. La controparte mp viene avviata insieme alla nota e con lo stesso indice round robin, così il movimento di Morph rimane continuo anche durante una nota tenuta.

## Struttura

Il motore audio gestisce campioni, round robin, le due varianti naturali, distorsione, synth dance e sintesi 808. Il wrapper JUCE collega il motore a MIDI, interfaccia e DAW. I dettagli essenziali sono in [docs/IMPLEMENTAZIONE.md](docs/IMPLEMENTAZIONE.md).

## Compilazione su Mac

Servono Xcode Command Line Tools, Git e CMake 3.22 o successivo. Il progetto usa **JUCE 8.0.4**; la prima configurazione richiede accesso a Internet per scaricarlo. Dalla cartella del progetto:

```sh
bash tools/build-macos.sh
```

Lo script compila il target `SculptBass_VST3` per Apple Silicon e Intel e prepara `dist/Sculpt-Bass-macOS-VST3.zip`. Il bundle ha una firma locale ad hoc; non è notarizzato. Per installarlo, copia `Sculpt Bass.vst3` in `~/Library/Audio/Plug-Ins/VST3/` e fai rileggere i plugin alla DAW.

## Stato

I controlli del progetto riguardano selezione campioni, round robin, morph, intonazione, ciclo delle note e assenza di allocazioni nel render, oltre alla sintassi del wrapper con gli header JUCE 8.0.4. Gli esiti aggiornati e i limiti della verifica sono in `docs/VERIFICA.txt`.

**La compilazione macOS e la verifica del VST3 in una DAW non sono ancora state eseguite. Questo è il pacchetto sorgenti, non un plugin già compilato.**

## Ascolto della revisione timbrica 4

`Preview/SculptBass-demo.wav` presenta lo stesso riff nelle cinque posizioni **0%, 25%, 50%, 75% e 100%**. Segue una nota tenuta mentre la macro attraversa il percorso in entrambe le direzioni, poi il rilascio. L'anteprima dura circa 35 secondi. È un rendering del motore reale con i campioni inclusi; i tempi indicativi sono in `Preview/ASCOLTO.txt`.

Note, velocity, tempistiche e gain del demo sono quelli della revisione 3, per confrontare direttamente i cambiamenti timbrici.

## Campioni

La base naturale usa 120 campioni fingerstyle **Darkblack** da [Black and Blue Basses](https://github.com/sfzinstruments/karoryfer.black-and-blue-basses), Karoryfer Samples, sotto licenza **CC0 1.0**: 15 note campionate, due dinamiche e quattro riprese round robin.
