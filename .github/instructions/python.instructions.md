---
applyTo: "custom_components/**/*.py,**/*.py"
---

# Règles Python — Custom Component Home Assistant

- Python 3.x avec annotations de type complètes
- Nommage : snake_case pour fonctions, variables et fichiers
- Suivre la structure standard HA : `__init__.py`, `config_flow.py`, `climate.py`, `const.py`, `manifest.json`
- Utiliser `bleak` pour toutes les communications BLE
- Les constantes BLE (UUIDs, handles) dans `const.py` avec commentaires explicatifs
- Logger avec `logging.getLogger(__name__)` — pas de `print()`
- Gérer les exceptions BLE explicitement (déconnexion, timeout, device introuvable)
- Compatibilité avec la version HA stable courante en priorité
- Documenter chaque caractéristique GATT avec son UUID et son rôle en commentaire
