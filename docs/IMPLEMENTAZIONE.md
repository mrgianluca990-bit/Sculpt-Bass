# Sculpt Bass — implementazione

## Obiettivo

Uno strumento immediato: suonare note MIDI e scegliere il timbro con una sola macro. Nella **versione 0.1.2, revisione timbrica 6**, il percorso è **naturale rotondo → naturale più aperto → distorto → synth dance → 808**, con passaggi continui.

Questa revisione riduce la saturazione del Distorto al 50%. Il pre-gain dello shaper passa da 24× a 9,28×, mantenendo il ramo saturo e usando la compensazione energetica già calcolata sulla banca reale per il nuovo drive. Naturali, Synth dance e 808 conservano i propri suoni della revisione 5.

La finestra riprende il layout di Sculpt Drums a **768 × 512**: titolo Sculpt Bass, grande Morph centrale, volume rotativo in basso a sinistra e VU a destra. Le tre etichette rimangono Naturale, Distorto e 808; il tooltip di Morph esplicita le cinque tappe timbriche. Dimensioni, posizioni, manopole e VU restano quelli di Drums.

## Motore

- **Naturale rotondo, 0%:** ripresa mp della selezione fingerstyle Darkblack di Black and Blue Basses, con un filtro leggero. Il carattere morbido parte dalla dinamica effettivamente registrata.
- **Naturale più aperto, 25%:** la ripresa mp resta prevalente, con un poco più di presenza e incisività e un attacco naturale.
- **Distorto, 50%:** saturazione più moderata, con corpo compensato e maggiore articolazione. La riduzione del drive entra gradualmente dopo il naturale e rientra nella curva precedente entro il 72% del morph; il contributo campionato del Synth al 75% conserva quindi il drive approvato.
- **Synth dance, 75%:** sintesi intonata alle note MIDI, con armoniche alte progressivamente attenuate e limitate in banda. La quota del basso distorto registrato scende dal 18% all'8%, conservando una parte dell'articolazione della corda. Il transiente sui bassi rimane presente; la componente alta dell'attacco è più contenuta.
- **808, 100%:** il corpo privilegia la fondamentale e poche armoniche moderate, al posto della lunga serie di armoniche dispari della revisione 4. Il rinforzo iniziale sui bassi rimane invariato, mentre il click alto è attenuato e più breve. Il gain del master e quello dello stadio 808 rimangono invariati.
- **Morph:** percorre progressivamente queste cinque tappe, con raccordi continui. Il movimento della manopola non riavvia la nota già in esecuzione.
- **Round robin:** alternanza automatica di quattro riprese reali per nota e dinamica. La controparte mp viene abbinata alla stessa nota campionata e allo stesso indice round robin.

Le cinque percentuali sono riferimenti sonori, non cambi di preset. Al note-on il motore seleziona la ripresa in base alla velocity MIDI e avvia anche la controparte mp corrispondente, quando distinta. Le due riprese rimangono disponibili per il morph della nota in corso. In questo modo anche una nota MIDI intensa può partire con il carattere naturale della ripresa mp, mentre il ramo distorto conserva la propria sorgente.

La macro deve cambiare il carattere mantenendo una continuità di livello ragionevole. La calibrazione dei singoli stadi accompagna la trasformazione timbrica. Il bilanciamento definitivo richiede ascolto nel plugin.

Il demo mantiene note, velocity, tempistiche e gain della revisione 5, per permettere un confronto diretto. Il motore segue i tempi MIDI ricevuti.

### Attacco e spettro dei sintetici, conservati dalla revisione 5

Il corpo Dance continua a usare una sintesi additiva limitata in banda, ma le armoniche superiori sfumano progressivamente. L'energia viene riequilibrata verso la fondamentale, così la pulizia dello spettro conserva il corpo della nota. Nell'808 restano la fondamentale e la seconda, terza, quinta, settima e nona armonica, con livelli moderati; la coda di armoniche superiori della revisione 4 viene rimossa.

Il rinforzo basso dell'attacco conserva la salita di 0,75 ms e il decadimento esponenziale di 12 ms. Il pacchetto armonico alto è più contenuto in entrambi gli stadi; verso l'808 il suo inviluppo diventa anche più breve. Frequenza, pitch-drop, fase e comportamento del legato restano quelli della revisione precedente.

Sulle note ribattute il transiente risale dal valore corrente; il legato non lo riavvia. La parte naturale segue il percorso precedente; la sola zona Distorto usa ora un drive più moderato con lo stesso trattamento antialiasing ADAA.

## Collegamento al plugin

`PluginProcessor` carica i campioni incorporati e collega il core `BassEngine` alla DAW. L'API usa `prepare` e `setSamples` per la preparazione, `noteOn` e `noteOff` per il MIDI, `setMorph` e `setOutputGain` per i controlli, `render` per generare l'audio.

I due parametri del plugin sono `morph` (0–100%) e `output` (−60…+6 dB). Sono salvati nello stato della DAW. Il wrapper gestisce anche pitch bend a ±2 semitoni e i messaggi MIDI di arresto delle note.

Identificatori del plugin, parametri e formato dello stato restano invariati; il numero di versione del progetto passa a 0.1.2.

## Campioni incorporati

La revisione 6 usa la stessa banca di **120 WAV: 15 note fra MIDI 23 e 64 × 2 dinamiche (mp/f) × 4 round robin**, mono PCM16 a 44,1 kHz. Manifest e WAV vengono incorporati con JUCE BinaryData: il plugin non dipende da una cartella campioni esterna.

La provenienza è fissata al commit `6e7d674cdb41be7a54dbccb15472401ad01099b9` di [Black and Blue Basses](https://github.com/sfzinstruments/karoryfer.black-and-blue-basses), licenza CC0 1.0. La mappatura usa l'altezza acustica verificata: una ottava sotto quella suggerita dai nomi originali; il campione denominato B1 è mappato a MIDI 23. `tools/factory_sources.json` conserva provenienza e hash originali; `Samples/manifest.json` descrive i file preparati.

## Limiti del prototipo

La base è un basso fingerstyle; non è una libreria completa di slap, plettro, slide o altre articolazioni. Entrambi i timbri naturali utilizzano queste riprese fingerstyle. La sezione 808 è un suono sintetico ispirato a quel carattere, non una riproduzione certificata di un apparecchio specifico.

Il round robin riduce la ripetizione identica dei campioni, ma non ricrea da solo il fraseggio di un bassista. Il comportamento della macro sulle note lunghe e la risposta ai diversi registri devono essere valutati all'ascolto.

La build macOS della revisione 6, il caricamento VST3 nella DAW e le prestazioni sul Mac dell'utente sono ancora da verificare. Il workflow usa `macos-15`; il controllo del binario universale usa `lipo "$sculpt_binary" -verify_arch arm64 x86_64`. I risultati Linux del core e la verifica degli script sono riportati separatamente in `VERIFICA.txt`.
