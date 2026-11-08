# Mise en route

## 1. Créer les jetons flespi

1. Créez un compte gratuit sur [flespi.io](https://flespi.io).
2. Dans *Tokens*, créez deux jetons de type **ACL**, un pour la carte et un pour l'iPhone. N'utilisez pas le jeton Master.
3. Sur chaque jeton, autorisez uniquement MQTT sur le topic `home/esp32lora/#`, en publication et en abonnement.
4. Sur le jeton de l'iPhone, limitez *Allowed origins* à `https://vnb-industries.github.io`.
5. Donnez aux deux jetons une date d'expiration lointaine (`expire`) et notez-la. Un jeton expiré est supprimé définitivement : il faudra alors en créer un autre, et reflasher la carte si c'est le sien.

## 2. Préparer les secrets

```bash
cp include/secrets.example.h include/secrets.h
```

Remplissez `include/secrets.h`, avec le jeton de la carte dans `CLOUD_MQTT_TOKEN`. Ce fichier est ignoré par Git. Générez les autres secrets avec :

```bash
openssl rand -hex 16
```

```bash
openssl rand -hex 32
```

La seconde commande sert à `CLOUD_COMMAND_KEY`.

## 3. Flasher la carte

La carte doit avoir son antenne vissée avant tout démarrage.

`uploadfs` conserve les volets et télécommandes de la carte : avant de construire l'image, `tools/prepare_fs.py` télécharge `1W.json` (clés d'appairage) et `RemoteMap.json` depuis la carte dans `config/`, puis les met dans l'image. Si la carte est injoignable, il reprend la copie déjà présente dans `config/`, et prévient s'il n'y en a aucune. `config/` n'est pas versionné : le dépôt est public.

```bash
pio run -e HeltecLoraV2ESP32 -t upload
```

```bash
pio run -e HeltecLoraV2ESP32 -t uploadfs
```

```bash
pio device monitor -b 115200
```

Le port série doit afficher `CloudMQTT: connected`, avec la mémoire disponible. Sans WiFi configuré, connectez-vous au point d'accès `iohc-setup` avec `LOCAL_AP_PASSWORD`.

## 4. Publier la webapp

1. Sur GitHub : *Settings*, *Pages*, *Source* : *GitHub Actions*.
2. Tout push sur `master` qui touche `webapp/` publie la nouvelle version.

Adresse : `https://vnb-industries.github.io/io-HomeController/`

## 5. Installer sur l'iPhone

1. Ouvrez l'adresse dans Safari, puis Partager, *Sur l'écran d'accueil*.
2. Ouvrez l'app depuis l'écran d'accueil. L'app installée ne partage pas les données de Safari.
3. Saisissez le jeton de l'iPhone et `CLOUD_COMMAND_KEY`.

## Sécurité

- Le broker relaie les messages sans pouvoir les signer : ni lui ni un jeton flespi volé ne suffisent à commander un volet.
- iPhone perdu : supprimez son jeton dans flespi, changez `CLOUD_COMMAND_KEY` dans `secrets.h`, et reflashez la carte.
- Le tableau de bord local demande `LOCAL_WEB_USER` et `LOCAL_WEB_PASSWORD`. Sans `include/secrets.h`, il n'a aucune protection : c'est le cas des binaires publiés par la CI.
- N'ouvrez aucun port sur la box.
- Le client MQTT d'origine (Home Assistant, port 1883, sans TLS) doit rester sur le réseau local.

## Tester une commande à la main

Pour vérifier les refus depuis un client MQTT ([MQTT Board](https://mqttboard.flespi.io), MQTT Explorer), calculez la signature ainsi :

```bash
printf 'c-test-1\n%s\nopen\na1b2c3\n' "$(date +%s)" | openssl dgst -sha256 -hmac "$CLOUD_COMMAND_KEY"
```

Le `ts` du message doit être la même valeur que celle de `date +%s`, à moins de 60 secondes près. Le détail du format est dans [le contrat MQTT](mqtt-contract.md).

## Tests de réception (en 4G, WiFi de l'iPhone coupé)

- [ ] La carte se connecte en TLS vérifié, publie `online` puis son état.
- [ ] L'app s'ouvre en plein écran depuis l'écran d'accueil.
- [ ] `open`, `close`, `stop`, le nom, le temps de course et le réessai fonctionnent, avec un `ack` en moins de 2 secondes.
- [ ] Une action inconnue signée, des paramètres invalides et une commande non signée reçoivent `ok: false`.
- [ ] Carte débranchée : l'app affiche « ESP32 hors ligne » en moins d'une minute et désactive les commandes.
- [ ] Box redémarrée : la carte se reconnecte seule.
- [ ] App en arrière-plan 10 minutes puis rouverte : reconnexion et état à jour.
- [ ] Le tableau de bord local demande un mot de passe, sa console, ses mises à jour et sa WebSocket fonctionnent.
- [ ] La radio fonctionne comme avant, y compris pendant une reconnexion.
- [ ] Mémoire : le minimum affiché par `CloudMQTT: connected` reste confortable après plusieurs reconnexions.
- [ ] Aucun secret dans l'historique Git ni dans les fichiers publiés.
- [ ] Aucune redirection de port sur la box.
