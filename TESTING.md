# Guide de test - ESPHome Madoka

## Prérequis

- ESPHome 2025.10+ (testé sur 2026.4)
- ESP32 ou ESP32-S3 avec le firmware flashé
- Thermostat Daikin Madoka BRC1H appairé avec l'ESP32

## Test 1 : Compilation

Dans le dashboard ESPHome, cliquer sur **Validate** puis **Install**.

La compilation doit passer sans erreur. Des warnings sur `ClimateTraits` indiquent une version ESPHome < 2026.4 (non bloquant).

## Test 2 : Connexion BLE

Dans les logs ESPHome, vérifier :

```
[D][ble_client]: Connected to XX:XX:XX:XX:XX:XX
[D][madoka]: Got update request...
[S][climate]: 'Madoka' >> Mode: OFF ...
```

## Test 3 : Entités dans Home Assistant

Vérifier que les entités suivantes apparaissent dans HA :

- `climate.madoka_*` — contrôle principal
- `sensor.madoka_*_temp_exterieure` — température extérieure
- `binary_sensor.madoka_*_filtre_a_nettoyer` — alerte filtre
- `text_sensor.madoka_*_firmware` — version firmware
- `number.madoka_*_luminosite_led` — luminosité LED (0-19)
- `button.madoka_*_reset_filtre` — acquittement alerte filtre

## Test 4 : Contrôle

Depuis HA, changer le mode (chaud, froid, etc.) et vérifier que le thermostat réagit.

## Test 5 : Switch de ré-appairage

1. Passer le switch **"Proxy Madoka Actif"** sur OFF
2. Vérifier dans les logs : `Arret BLE - scan stop et deconnexion des thermostats`
3. L'application Daikin doit pouvoir se connecter au thermostat
4. Repasser sur ON et vérifier la reconnexion automatique
