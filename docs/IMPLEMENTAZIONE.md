# Sculpt Bass — implementazione

## Obiettivo

Uno strumento immediato: suonare note MIDI e scegliere il timbro con una sola macro. Nella **versione 0.1.0, revisione timbrica 4**, il percorso è **naturale rotondo → naturale più aperto → distorto → synth dance → 808 aggressivo**, con passaggi continui.

Questa revisione interviene sull'attacco e sul contenuto armonico di Dance e 808. I percorsi dei due naturali e del distorto mantengono il comportamento della revisione 3.

La finestra riprende il layout di Sculpt Drums a **768 × 512**: titolo Sculpt Bass, grande Morph centrale, volume rotativo in basso a sinistra e VU a destra. Le tre etichette rimangono Naturale, Distorto e 808; il tooltip di Morph esplicita le cinque tappe timbriche. Dimensioni, posizioni, manopole e VU restano quelli di Drums.

## Motore

- **Naturale rotondo, 0%:** ripresa mp della selezione fingerstyle Darkblack di Black and Blue Basses, con un filtro leggero. Il carattere morbido parte dalla dinamica effettivamente registrata.
- **Naturale più aperto, 25%:** la ripresa mp resta prevalente, con un poco più di presenza e incisività e un attacco naturale.
- **Distorto, 50%:** conserva il percorso di elaborazione e il carattere approvati nella revisione precedente.
- **Synth dance, 75%:** sintesi intonata alle note MIDI con armoniche più forti ed estese anche nel corpo della nota, limitate in banda per restare entro la frequenza di Nyquist. Un transiente breve con salita smussata e apertura armonica dedicata rende l'attacco più percussivo. La quota del basso distorto registrato conserva parte dell'articolazione della corda.
- **808 aggressivo, 100%:** un transiente breve con salita smussata e apertura armonica dedicata rende più marcato l'attacco. Il corpo mantiene una fondamentale forte e presenta una serie di armoniche più ricca ed estesa. Il gain generale rimane quello della revisione precedente.
- **Morph:** percorre progressivamente queste cinque tappe, con raccordi continui. Il movimento della manopola non riavvia la nota già in esecuzione.
- **Round robin:** alternanza automatica di quattro riprese reali per nota e dinamica. La controparte mp viene abbinata alla stessa nota campionata e allo stesso indice round robin.

Le cinque percentuali sono riferimenti sonori, non cambi di preset. Al note-on il motore seleziona la ripresa in base alla velocity MIDI e avvia anche la controparte mp corrispondente, quando distinta. Le due riprese rimangono disponibili per il morph della nota in corso. In questo modo anche una nota MIDI intensa può partire con il carattere naturale della ripresa mp, mentre il ramo distorto conserva la propria sorgente.

La macro deve cambiare il carattere mantenendo una continuità di livello ragionevole. La calibrazione dei singoli stadi accompagna la trasformazione timbrica. Il bilanciamento definitivo richiede ascolto nel plugin.

Il demo mantiene note, velocity, tempistiche e gain della revisione 3, per permettere un confronto diretto. Il motore segue i tempi MIDI ricevuti.

### Attacco e spettro della revisione 4

La salita del transiente è di 0,75 ms e il suo decadimento esponenziale di 12 ms. Il rinforzo iniziale combina la fondamentale con un pacchetto armonico separato, sincronizzato alla stessa fase. Il corpo Dance usa fino a 64 parziali; l’808 mantiene una coda di armoniche dispari fino alla 63ª. Le armoniche alte sfumano prima di Nyquist.

Sulle note ribattute il transiente risale dal valore corrente; il legato non lo riavvia. La parte naturale e il distorto al 50% seguono il percorso precedente.

## Collegamento al plugin

`PluginProcessor` carica i campioni incorporati e collega il core `BassEngine` alla DAW. L'API usa `prepare` e `setSamples` per la preparazione, `noteOn` e `noteOff` per il MIDI, `setMorph` e `setOutputGain` per i controlli, `render` per generare l'audio.

I due parametri del plugin sono `morph` (0–100%) e `output` (−60…+6 dB). Sono salvati nello stato della DAW. Il wrapper gestisce anche pitch bend a ±2 semitoni e i messaggi MIDI di arresto delle note.

## Campioni incorporati

La revisione 4 usa la stessa banca di **120 WAV: 15 note fra MIDI 23 e 64 × 2 dinamiche (mp/f) × 4 round robin**, mono PCM16 a 44,1 kHz. Manifest e WAV vengono incorporati con JUCE BinaryData: il plugin non dipende da una cartella campioni esterna.

La provenienza è fissata al commit `6e7d674cdb41be7a54dbccb15472401ad01099b9` di [Black and Blue Basses](https://github.com/sfzinstruments/karoryfer.black-and-blue-basses), licenza CC0 1.0. La mappatura usa l'altezza acustica verificata: una ottava sotto quella suggerita dai nomi originali; il campione denominato B1 è mappato a MIDI 23. `tools/factory_sources.json` conserva provenienza e hash originali; `Samples/manifest.json` descrive i file preparati.

## Limiti del prototipo

La base è un basso fingerstyle; non è una libreria completa di slap, plettro, slide o altre articolazioni. Entrambi i timbri naturali utilizzano queste riprese fingerstyle. La sezione 808 è un suono sintetico ispirato a quel carattere, non una riproduzione certificata di un apparecchio specifico.

Il round robin riduce la ripetizione identica dei campioni, ma non ricrea da solo il fraseggio di un bassista. Il comportamento della macro sulle note lunghe e la risposta ai diversi registri devono essere valutati all'ascolto.

La build macOS, il caricamento VST3 nella DAW e le prestazioni sul Mac dell'utente sono ancora da verificare. I controlli del core, quando eseguiti, non sostituiscono queste verifiche.
