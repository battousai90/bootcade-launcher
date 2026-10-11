// src/RetroAchievements.h : le compte RetroAchievements du joueur.
#pragma once

#include <string>
#include <vector>

/* RetroAchievements vit dans FinalBurn Neo (rcheevos, dans notre fork) : c'est
 * l'emulateur qui lit la memoire du jeu et debloque les succes. Le launcher ne
 * fait que trois choses :
 *  - tenir le compte : le joueur se connecte une fois, on garde le jeton
 *    (jamais le mot de passe), dans son propre fichier en 0600 ;
 *  - le passer a FinalBurn Neo au lancement (variables d'environnement) ;
 *  - montrer, dans le volet de details, les succes d'un jeu et ceux debloques.
 *
 * Seuls les jeux d'arcade de FinalBurn Neo sont concernes : RetroAchievements
 * les reconnait par le nom du set, et notre fork expose leur memoire comme le
 * core libretro sur lequel les succes ont ete faits. */
namespace RetroAchievements {

bool signed_in();
std::string username();
bool hardcore();                      // faux par defaut
void set_hardcore(bool on);

// Bloquant. Rend un message d'erreur lisible, ou "" si la connexion a reussi.
std::string login(const std::string& user, const std::string& password);
void sign_out();

// Les variables a donner a FinalBurn Neo, vide si personne n'est connecte.
std::vector<std::string> launch_env();

// Les jeux que RetroAchievements peut reconnaitre ici.
bool supports(const std::string& emulator, const std::string& system);

struct Achievement {
    unsigned    id = 0;
    std::string title;
    std::string description;
    unsigned    points = 0;
    bool        unlocked = false;
};

struct GameAchievements {
    bool        answered = false;     // le serveur a repondu
    std::string error;                // si answered est faux
    unsigned    game_id = 0;          // 0 : jeu inconnu de RetroAchievements
    std::vector<Achievement> list;    // succes officiels seulement
    // Avertissement que le serveur glisse parmi les succes (identifiant
    // 101000001 et plus), ex. « Unknown Emulator : Hardcore unlocks cannot be
    // earned using this emulator ». Titre + explication, vide s'il n'y en a pas.
    std::string warning;
    unsigned    unlocked = 0;
    unsigned    points = 0, points_unlocked = 0;
};

// Bloquant : a appeler hors du fil graphique.
GameAchievements fetch(const std::string& set_name);

/* Les jeux d'arcade qui ont des succes, et combien.
 *
 * Une requete publique (sans compte) rend tout le catalogue Arcade de
 * RetroAchievements avec les empreintes des noms de sets : de quoi poser la
 * pastille, le filtre et l'option de Random play sur la liste entiere, sans
 * une requete par jeu. Garde dans retroachievements-arcade.json et relu au
 * demarrage ; rafraichi au plus une fois par jour. */
// Nombre de succes officiels du set, 0 s'il n'en a pas (ou catalogue absent).
unsigned achievement_count(const std::string& emulator, const std::string& system,
                           const std::string& set_name);
/* Relie le compte RetroAchievements au compte Bootcade.
 *
 * Le NOM du compte (jamais le jeton) est ecrit dans l'attribut Keycloak
 * « retroachievements » du joueur, par l'API de compte de Keycloak, avec le
 * jeton Bootcade : le site le lit dans le jeton pour l'afficher sur le profil.
 * Efface quand le joueur se deconnecte de RetroAchievements. Sans compte
 * Bootcade, ne fait rien. Bloquant : hors du fil graphique. Rend "" si
 * l'attribut est a jour, sinon l'erreur. */
std::string sync_bootcade_link();

// Telecharge le catalogue s'il date de plus d'un jour. Bloquant, hors du fil
// graphique. Rend true si le catalogue a change.
bool refresh_catalog();

}  // namespace RetroAchievements
