---
applyTo: "esphome/**,**/*.yaml,**/*.yml"
---

# Règles ESPHome — Configuration et lambdas C++

- YAML ESPHome valide, commentaires en français
- Plateforme cible : ESP32 avec Bluetooth intégré
- Utiliser le composant `ble_client` d'ESPHome pour la communication BLE
- Les adresses MAC et UUIDs en constantes nommées (pas de valeurs hardcodées anonymes)
- Lambdas C++ : code minimal, documenté, sans dépendances externes
- Préfixer les entités ESPHome avec `madoka_` pour éviter les conflits
- Toujours inclure les sections `logger`, `api`, `ota` dans les configurations complètes
- Tester la configuration avec `esphome compile` avant de proposer
