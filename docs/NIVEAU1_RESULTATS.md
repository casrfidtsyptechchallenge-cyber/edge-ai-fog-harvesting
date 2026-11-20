# Niveau 1 de l'IA : ce qui a été fait, mesuré et décidé

Tous les chiffres viennent de **données simulées** (sauf la prévision du brouillard, une fois lancée sur les
vrais fichiers METAR). Chaque décision a été prise sur des graines de réglage, puis vérifiée sur des graines jamais
utilisées.

## 1-1 Détecteur de situation inconnue : livré dans le firmware

L'IA signale quand une fenêtre de courant ne ressemble pas à ses données d'entraînement. Trois vérifications :
saturation de l'ADC, plage physique (échelle de courant, bruit, impulsions), forme (distance de Mahalanobis
sur les 32 valeurs internes du CNN).

| Situation | Fenêtres signalées |
|---|---|
| Données normales | 1,0 % (objectif fixé) |
| Capteur débranché | 100 % |
| Module à faible courant (×0,1) | 51 % |
| Bruit ×3 | 35 % |
| Écrêtage de l'ADC | 21 % (= les fenêtres réellement écrêtées) |
| Impulsions 3× plus petites | 0,6 % : **non vu** (limite assumée) |

- C++ embarqué = Python sur 10 000 fenêtres (100 % de décisions identiques).
- Coût : +1,6 % d'instructions avec FPU, 4,7 KB de flash.
- Sur le nœud : fenêtres `wave` NOVEL pour M11, champ `arc_novel` dans la télémétrie (contrat 1.3), et M6 qui
  ne monte plus quand l'IA est dans l'inconnu.
- Décision écartée sur mesures : basculer sur la règle quand c'est inconnu (cela dégradait les résultats).
- Découverte en route : un capteur débranché lit ≈ 0 µA. M3 a maintenant un test de plausibilité
  (`AEROION_HAS_HV_OUTPUT` à mettre à 1 quand l'étage HT est câblé).

## 1-2 Apprentissage sur le nœud et fédéré : expérience, pas dans le firmware

- Découverte principale : le **biais d'intervention**. Après un BACKOFF déclenché par une prédiction, « pas
  d'arc » ne prouve rien. Votre pipeline M11 avait ce biais : il est corrigé (colonne `intervened` dans M2,
  fenêtres écartées dans M11).
- Mode `ai observe` sur le MCX : pour les campagnes en chambre, le modèle calcule sans que M3 agisse (la
  détection d'arc à 250 µA reste active). Ce sont les meilleures données d'entraînement possibles.
- Apprentissage embarqué testé en 6 versions : les deux premières étaient dangereuses (mesuré), la dernière est
  sûre. Sur le nœud décalé, −3,6 fautes/h et −27 arcs/h, avec une meilleure calibration. Mais l'effet sur l'eau
  varie d'une graine à l'autre : **pas activé dans le firmware**.

## 1-3 Prévision du brouillard : pipeline prêt, données à télécharger

`python -m ml fog-forecast fichiers.csv`. Aucun chiffre n'est revendiqué avant les vrais fichiers METAR
(Tunisia ASOS, archive de l'Iowa State University). Le modèle n'utilise que ce que mesure un nœud
(température, humidité, heure, saison). Un second modèle avec le vent chiffre l'apport d'un anémomètre.

## Tests

279 tests Python, 208 tests C++ (sous ASan/UBSan aussi), compilation Cortex-M33 : tout est vert.
