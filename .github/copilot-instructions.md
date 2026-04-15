# Instructions Copilot — daikin_madoka

> Complément à AGENTS.md, spécifique à GitHub Copilot.
> Les règles générales sont dans AGENTS.md à la racine.

## Comportement attendu

- Toujours consulter AGENTS.md pour le contexte BLE et la stack
- Pour le code Python HA : suivre la structure `custom_components/` standard
- Pour ESPHome : générer du YAML valide avec lambdas C++ si nécessaire
- Documenter systématiquement les UUIDs et octets BLE dans les commentaires
- Ne pas suggérer de dépendances non standard pour HA (préférer ce qui est déjà intégré)
