# Appairage des télécommandes

Le boîtier se comporte comme une télécommande io-homecontrol 1W de plus. Il s'ajoute au volet à côté des télécommandes existantes, sans les remplacer.

Il faut une télécommande déjà appairée au volet (bouton PROG au dos).

## 1. Créer l'appareil

Interface web (http://miopenio.local), onglet Appareils, « Ajouter appareil », saisir un nom.

Ou en série :

```
new1W Salon
list1W
```

`list1W` affiche la description de 4 lettres (ex. `QWER`), à utiliser dans les commandes série.

## 2. Associer au volet

1. Sur la télécommande existante, appuyer sur PROG environ 2 s, jusqu'au bref va-et-vient du volet.
2. Dans la minute qui suit, ouvrir l'appareil dans l'interface et cliquer « Associer ». En série : `add QWER`.
3. Le volet refait un va-et-vient : c'est appairé.
4. Tester avec Ouvrir / Fermer, ou `open QWER`.

Un seul volet doit être en mode PROG à la fois, sinon tous ceux en mode PROG sont appairés.

## 3. Dissocier

Même chose : PROG sur la télécommande existante, puis « Dissocier ». En série : `remove QWER`.

Dissocier avant de supprimer l'appareil (`del1W QWER`). Sinon le volet garde une télécommande fantôme.

## Ajouter une autre télécommande depuis le boîtier

Une fois le boîtier appairé, `pair QWER` joue le rôle du bouton PROG. Le volet passe en mode appairage et accepte une nouvelle télécommande.

## Nommer une télécommande physique

Facultatif. Le nom remplace l'adresse dans le journal et sur l'écran.

1. Appuyer sur un bouton de la télécommande.
2. Lire l'adresse dans le journal (`Command received from 1A2B3C`) ou avec `lastAddr`.
3. Onglet Télécommandes, ajouter avec cette adresse (pré-remplie). En série : `newRemote 1A2B3C Télécommande salon`.
4. Lier les volets qu'elle commande : `linkRemote 1A2B3C QWER`.

## Sauvegarde

Les clés d'appairage sont dans `1W.json`. `uploadfs` écrase ce fichier. Faire « Download Devices » avant toute mise à jour du système de fichiers.
