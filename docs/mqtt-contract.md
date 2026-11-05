# Contrat MQTT de la webapp

La webapp et la carte communiquent par un broker MQTT cloud. Elles ouvrent chacune une connexion sortante chiffrée. Aucun port n'est ouvert sur la box.

| Usage | Protocole | Adresse |
| --- | --- | --- |
| Carte | MQTT sur TLS, certificat vérifié (ISRG Root X1) | `<cluster>:8883` |
| Webapp | MQTT sur WebSocket TLS | `wss://<cluster>:8884/mqtt` |

## Topics

Le préfixe est `home/esp32lora`. Il est défini dans `src/cloud_mqtt.cpp` et dans `webapp/app.js`, et doit rester identique des deux côtés.

| Topic | Sens | QoS | Retained | Contenu |
| --- | --- | --- | --- | --- |
| `home/esp32lora/cmd` | webapp vers carte | 1 | non | Commande signée |
| `home/esp32lora/ack` | carte vers webapp | 1 | non | Résultat d'une commande |
| `home/esp32lora/state` | carte vers webapp | 1 | oui | État complet |
| `home/esp32lora/status` | carte vers webapp | 1 | oui | Texte `online` ou `offline` (Last Will) |

## Commande

```json
{ "id": "c-1730822400123-42", "action": "open", "params": { "deviceId": "a1b2c3" }, "ts": 1730822400, "sig": "<64 caractères hexadécimaux>" }
```

| `action` | `params` | Effet |
| --- | --- | --- |
| `open`, `close`, `stop` | `deviceId` | Pilote le volet |
| `time1W` | `deviceId`, `seconds` (1 à 600) | Règle le temps de course |
| `repeat1W` | `deviceId`, `enabled` (booléen) | Réessaie si le volet ne répond pas |
| `edit1W` | `deviceId`, `name` (1 à 32 octets, sans caractère de contrôle) | Renomme le volet |

L'appairage, la suppression, les réglages de la carte et la console restent accessibles uniquement en local.

## Signature

`sig` est un HMAC-SHA256 en hexadécimal minuscule, calculé avec `CLOUD_COMMAND_KEY` sur ces cinq lignes séparées par `\n`, sans retour à la ligne final :

```
id
ts
action
deviceId
valeur
```

`valeur` vaut `seconds` en décimal, `1` ou `0` pour `enabled`, `name`, ou une chaîne vide pour les autres commandes.

## Ordre des contrôles sur la carte

1. Le message fait au plus 512 octets, le JSON est valide et `id` fait 1 à 64 caractères.
2. La signature est valide.
3. `id` ne figure pas parmi les 16 derniers traités. Sinon la commande est ignorée sans `ack`.
4. `ts` est à moins de 60 secondes de l'heure de la carte.
5. `action` et `params` passent la liste blanche de `handleCommand` (`src/device_commands.cpp`).

Toute commande refusée à une de ces étapes reçoit `ok: false`, sauf le doublon de l'étape 3. Rien n'est exécuté. La session est propre : le broker ne garde aucune commande pendant que la carte est hors ligne.

## Accusé de réception

```json
{ "id": "c-1730822400123-42", "ok": true, "error": null }
```

`ok: true` signifie que la commande a été envoyée par radio. Les moteurs 1W ne confirment rien.

## État

Il est publié à la connexion, après chaque commande, quand les volets changent (contrôle toutes les 5 secondes) et au moins toutes les 60 secondes.

```json
{
  "uptime_s": 3600,
  "rssi_wifi": -61,
  "fw": "1.0.0",
  "values": {
    "devices": [
      { "id": "a1b2c3", "name": "Salon", "position": 40, "paired": true, "travel_time": 25, "repeat": false }
    ]
  }
}
```

`position` est une estimation. Les clés d'appairage ne sont jamais publiées.
