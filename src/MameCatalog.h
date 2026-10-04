// src/MameCatalog.h
//
// Le catalogue MAME n'est pas importe, il est interroge.
//
// `mame -listxml` sort 320 Mo et 371 752 <rom> : les recopier dans games.db
// reviendrait a entretenir une deuxieme verite, forcement en retard sur celle
// de l'emulateur installe. On ne garde donc que ce que l'interface affiche
// (nom, description, annee, fabricant, parent, fichier source, etat du pilote),
// soit 14 Mo lus en 2,5 s, et on redemande le reste a MAME quand un ecran en a
// besoin : `mame -listroms <machine>` repond en 45 ms.
//
// Le cache est invalide par l'attribut build= de MAME : une mise a jour de la
// distribution suffit a declencher une regeneration, et il n'y a aucun DAT a
// telecharger ni a tenir a jour.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Game.h"

class DatabaseManager;

namespace MameCatalog {

// Le chemin de l'executable MAME, ou une chaine vide s'il est introuvable.
// Cherche dans le PATH de l'hote : sous Flatpak, MAME vit en dehors du bac a
// sable, comme FinalBurn Neo.
std::string find_executable();

// Vrai si `exe` est un executable que Bootcade peut lancer. Sous Flatpak, le
// MAME de la distribution (/usr/games/mame) n'existe pas dans le bac a sable :
// un access() local le declarait absent alors que le jeu se lancait sur
// l'hote. On pose donc la question a l'hote.
bool is_runnable(const std::string& exe);

// La version annoncee par le binaire, p.ex. "0.289 (unknown)". Chaine vide si
// MAME ne repond pas. C'est la cle d'invalidation du cache : elle est gardee
// telle quelle, suffixe compris, pour qu'un meme MAME ne se mette jamais a
// ressembler a un autre et ne declenche pas de regeneration inutile.
std::string installed_build(const std::string& mame_exe);

// Le seul numero de version, "0.289" pour "0.289 (unknown)".
//
// Ce qui suit le numero est l'identifiant de la revision construite, que les
// paquets des distributions ne renseignent pas : MAME ecrit alors le mot
// « unknown », qui se lit a l'ecran comme une panne alors que tout va bien.
std::string version_number(const std::string& raw);

// La derniere version publiee par MAMEdev, lue sur la page des sorties.
//
// MAMEdev ne publie AUCUN binaire Linux, seulement les sources : on ne peut
// donc que signaler l'ecart et dire ou regarder, jamais proposer de
// telecharger quoi que ce soit comme on le fait pour FinalBurn Neo.
struct LatestRelease {
    bool ok = false;         // false des qu'on n'a pas su lire un numero
    std::string version;     // "0.290"
    std::string error;       // renseigne quand ok == false
};
LatestRelease fetch_latest_release();

// Compare deux numeros facon MAME ("0.289"). Rend <0, 0 ou >0. Un numero
// illisible rend 0 : on prefere ne rien affirmer a affirmer n'importe quoi.
int compare_versions(const std::string& a, const std::string& b);

// La version pour laquelle le cache a ete construit, telle qu'enregistree.
std::string cached_build(const std::shared_ptr<DatabaseManager>& db);

// Relit le catalogue depuis MAME et remplace la table. Rend le nombre de
// machines enregistrees, ou -1 en cas d'echec. `progress` est appele de temps
// en temps avec le compte courant, jamais depuis un autre fil que l'appelant.
int rebuild(const std::shared_ptr<DatabaseManager>& db,
            const std::string& mame_exe,
            const std::function<void(int)>& progress = {});

// Regenere seulement si la version installee differe de celle du cache.
// Rend le nombre de machines disponibles ensuite.
int sync(const std::shared_ptr<DatabaseManager>& db,
         const std::string& mame_exe,
         const std::function<void(int)>& progress = {});

// Les dossiers de ROMs declares dans ~/.mame/mame.ini, pour les proposer a
// l'utilisateur. Chaque entree est rendue telle quelle, y compris si elle
// n'existe plus : c'est justement ce qu'il faut lui montrer.
std::vector<std::string> rompaths_from_mame_ini();

// Verdict de MAME sur la collection, ecrit dans le catalogue.
//
// On ne reimplemente pas la regle : `mame -verifyroms` connait les sets
// splits, les BIOS et les peripheriques mieux que nous, et il ne peut pas
// etre en desaccord avec lui-meme. Sa sortie est simplement traduite dans le
// vocabulaire du launcher.
//
// Les sets absents ne produisent aucune ligne : ils se deduisent par
// difference avec le catalogue. Rend le nombre de sets trouves, ou -1.
struct AuditResult {
    int good = 0;        // "is good"
    int playable = 0;    // "is best available" : jouable, dumps manquants inconnus
    int bad = 0;         // "is bad" : present mais abime
    int missing = 0;     // jamais mentionne par MAME
};
AuditResult audit(const std::shared_ptr<DatabaseManager>& db,
                  const std::string& mame_exe,
                  const std::vector<std::string>& rompaths,
                  const std::function<bool(int)>& progress = {});

// Ecrit, a partir de `mame -listxml`, les trois DAT au decoupage de
// Pleasuredome : « MAME ROMs (split).dat », « MAME ROMs (bios-devices).dat »
// et « MAME CHDs (merged).dat ».
//
// Le catalogue en cache suffit a afficher la bibliotheque, mais pas au
// gestionnaire de ROMs : celui-ci compare fichier par fichier, et toute sa
// machinerie — DatParser, RomResolve, l'audit — travaille sur des DAT Logiqx.
//
// Chaque machine y dit ce que contient son zip (ou son dossier de CHD), sans
// aucun lien : memes en-tetes, meme contenu que les DAT Pleasuredome de la
// meme version de MAME (verifie a zero ecart sur la 0.289), donc memes
// dossiers attendus. Les noms de fichiers ne portent pas la version : une
// nouvelle generation remplace la precedente, et la selection du groupe
// reste valable. La version (« 0.289 », ou « 0.289-dev.1306+g8aeb28649e »
// pour un MAME de developpement) est dans l'en-tete.
//
// Remplace les fichiers d'une generation precedente signes par Bootcade
// (l'ancien format MAME_-_Arcade.dat / MAME_-_Mechanical.dat, le DAT unique
// « MAME <ver>.dat ») — sans quoi « Update DAT » chargerait les memes
// machines deux fois. Un DAT depose par l'utilisateur n'est jamais touche.
//
// Rend le nombre de fichiers ecrits (3), ou -1 en cas d'echec. `progress`
// recoit le nombre de machines lues ; rendre false l'interrompt.
//
// kHeader : l'en-tete sous lequel un -listxml brut, fourni tel quel comme DAT,
// est lu (read_listxml_file).
constexpr const char* kHeader = "MAME";

int generate_dats(const std::string& mame_exe,
                  const std::string& dat_dir,
                  const std::function<bool(int)>& progress = {});

// La meme chose depuis un fichier -listxml deja ecrit (celui que publie
// progettosnaps, racine <mame build=…>) : lu en flux, jamais charge en
// entier. Ecrit les trois DAT dans `out_dir` sans rien y supprimer d'autre.
struct ConvertResult {
    std::string version;                 // celle des en-tetes : « 0.289 », « 0.289-dev.1306+g… »
    int sets = 0;                        // sets ecrits
    int machines = 0;                    // machines lues
    std::vector<std::string> files;      // les chemins ecrits
};
int convert_listxml_file(const std::string& xml_path, const std::string& out_dir,
                         const std::function<bool(int)>& progress = {},
                         ConvertResult* result = nullptr);

// Un fichier -listxml fourni comme DAT : lu directement, en flux, sans DAT
// intermediaire. Chaque machine qui a une ROM ou un disque devient un set,
// avec ses liens (cloneof, romof), merge= et ses device_ref, exactement ce
// que le DAT de generate_dats en dirait. Ajoute a `out` ; rend le nombre de
// sets lus, -1 en cas d'echec.
int read_listxml_file(const std::string& xml_path, std::vector<Game>& out,
                      std::string* version = nullptr,
                      const std::function<bool(int)>& progress = {});

/* ── Les genres, qui ne viennent pas de MAME ──────────────────────────────
 *
 * `mame -listxml` n'expose ni genre, ni famille, ni nombre de joueurs : ces
 * champs sont une extension de notre fork FinalBurn Neo, ecrite dans ses DAT.
 * Le filtre « Genre » ne montrait donc que du FinalBurn Neo, non par panne
 * mais faute de donnee.
 *
 * catver.ini, le fichier communautaire de progetto-SNAPS, classe les machines
 * MAME par categorie et couvre la totalite des machines jouables. On ne le
 * fabrique pas et on ne le devine pas : on le lit, ou on le telecharge.
 */
struct CatverResult {
    bool ok = false;
    int  entries = 0;       // lignes lues dans [Category]
    int  applied = 0;       // machines du catalogue effectivement classees
    std::string path;       // le fichier retenu ou ecrit
    std::string error;      // renseigne quand ok == false
};

// L'adresse de l'archive pour cette version de MAME. Le nom de fichier porte
// le numero sans le point (« 0.289 » -> « 289 »). Chaine VIDE si la version
// n'a pas la forme attendue : inventer un numero ramenerait le catver d'une
// autre version de MAME sans que personne ne s'en apercoive.
std::string catver_url(const std::string& mame_version);

// Ou l'application range sa copie : <dossier de configuration>/catver.ini.
std::string catver_app_path();

// Le fichier a lire : celui que l'utilisateur a designe dans les reglages,
// sinon la copie de l'application. Vide si aucun des deux n'existe.
std::string catver_path(const std::string& configured);

// Lit un catver.ini et pose les genres sur le catalogue. Ne touche a rien
// d'autre : le catalogue reste celui de MAME.
CatverResult apply_catver(const std::shared_ptr<DatabaseManager>& db,
                          const std::string& path);

// Telecharge l'archive et en extrait le seul catver.ini, dans `dest_dir`.
// `progress` recoit l'avancement entre 0 et 1 ; rendre false interrompt.
CatverResult download_catver(const std::string& url,
                             const std::string& dest_dir,
                             const std::function<bool(double)>& progress = {});

/* ── Savoir s'il existe un catver.ini plus recent ─────────────────────────
 *
 * progetto-SNAPS ne publie aucun manifeste : rien a lire qui dise « la
 * derniere version est celle-ci ». Mais son nom de fichier porte le numero
 * de MAME, et le fichier lui-meme le redit dans son en-tete
 * (« ;; catver.ini 0.289 / 21-Aug-26 / MAME 0.289 ;; »). La verification
 * compare donc ce que l'on a a ce que le serveur accepte encore de servir,
 * en demandant les quatre premiers octets des versions suivantes.
 *
 * Elle ne devine jamais : quand l'adresse ne porte pas de numero, ou que le
 * serveur ne repond pas, la reponse le DIT au lieu d'annoncer « a jour ».
 */

// La version qu'un catver.ini declare dans son en-tete (« 0.289 »), vide
// quand le fichier n'en porte pas. C'est la source la plus fiable : le nom
// du fichier, lui, a pu etre choisi par n'importe qui.
std::string catver_file_version(const std::string& path);

// Le numero a trois chiffres que porte une adresse de telechargement
// (« pS_CatVer_289.zip » -> 289), -1 quand elle n'en porte aucun.
int catver_url_version(const std::string& url);

// La meme adresse, pour une autre version. Vide si l'adresse n'a pas de
// numero a remplacer.
std::string catver_url_with_version(const std::string& url, int version);

// Present : le serveur sert bien une archive a cette adresse. Absent : il
// repond mais n'a pas ce fichier. Unreachable : on n'a pas pu lui demander,
// ce qui n'autorise a conclure ni dans un sens ni dans l'autre.
enum class UrlProbe { Present, Absent, Unreachable };
UrlProbe probe_catver_url(const std::string& url);

struct CatverCheck {
    bool        asked     = false;  // le serveur a repondu au moins une fois
    int         local     = -1;     // version du fichier present ici
    int         newest    = -1;     // la plus recente version trouvee en ligne
    int         probed_to = -1;     // jusqu'ou on a regarde
    std::string url;                // l'adresse de `newest`
    std::string error;              // renseigne quand asked == false
};

// Demande au serveur, en partant de la version que l'on a (ou de celle de
// l'adresse, a defaut), si les suivantes existent. S'arrete a deux absences
// consecutives : progetto-SNAPS publie une archive par version de MAME, sans
// trou, et sonder indefiniment ferait une dizaine de requetes pour rien.
CatverCheck check_catver(const std::string& url, const std::string& local_file);

// Le catalogue en memoire, sous la forme que l'interface manipule deja.
// Les machines purement internes (isdevice) sont ecartees : ce sont de vraies
// archives, necessaires a l'audit, mais personne ne les « joue ».
std::vector<Game> load(const std::shared_ptr<DatabaseManager>& db,
                       bool include_mechanical = false);

}  // namespace MameCatalog
