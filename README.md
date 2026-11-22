# ONIRIQUE DE-ESSER (JUCE 8 / VST3, AU)

Le DE-ESSER de VOCALBOX en plugin seul, avec deux modes.

- **AUTO** : un de-esser classique réglé par un THRESHOLD (ligne qu'on glisse dans la vue). S (à la fréquence FREQ), T, CH, F
  cochés ; RANGE (réduction max), CLARITY (l'éclat rendu sans les S), LISTEN (seulement ce qui est retiré). La vue montre ce
  qui est retiré, son par son, avec la lettre (S, T, CH, F) au-dessus de chaque retrait.
- **MANUEL** : CAPTURE enregistre la prise pendant que le DAW la joue ; elle est découpée en événements (SIBILANCES S Z CH F,
  CONSONNES P B T K, RESPIRATIONS) réglés un par un : vue en COLONNES ou TIMELINE, survol = écoute, glisser = baisser,
  double-clic = rendu à TOUT RÉDUIRE (une dose par famille, les plus saillants davantage). Le gain est appliqué pendant la
  lecture à la position du DAW (fondus 5 ms, 20 ms pour une respiration) ; la latence est la même dans les deux modes.

Sources : `Source/dsp/AutoDeEsser.h` (moteur AUTO), `EventAnalysis.h` (détection), `Capture.h` (capture, sauvegarde),
`ManualEngine.h` (gains par événement, écoute), `Source/ui/DeessViews.h` (vues).
Bancs : `Tests/EngineBench.cpp` (dans la CI Windows), `Tests/DetectBench.cpp <voix.wav> [spectre.png]`.
La prise capturée est enregistrée à part : `%APPDATA%/Onirique/ONIRIQUE DE-ESSER/Captures/<id>.ocap` (le projet garde l'id).
