// src/Game.h
#pragma once
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

/* Un seul vocabulaire de genres pour les deux catalogues.
 *
 * FinalBurn Neo et MAME classent leurs jeux chacun de son cote, et sans se
 * concerter : le premier ecrit « Racing », « Platformer », « Fighting /
 * Versus » ; le second « Driving », « Platform », « Fighter ». Ce sont les
 * memes jeux sous deux noms, et laisser les deux dans la colonne des filtres
 * obligeait a cliquer deux fois pour voir tous les jeux de course.
 *
 * La table ci-dessous est un jugement humain, relu entree par entree : elle ne
 * couvre QUE les equivalences evidentes. Tout ce qui decrit du materiel plutot
 * qu'un genre — « Computer », « Slot Machine », « Handheld », « Tabletop » —
 * reste tel quel : MAME emule des machines, pas seulement des bornes, et
 * forcer ces categories dans le vocabulaire de FBNeo n'aurait aucun sens.
 *
 * « Ball & Paddle » et « Climbing » sont gardes comme categories, et ce sont
 * les « Breakout » de FinalBurn Neo qui les rejoignent : la categorie de MAME
 * est la plus large des deux (elle couvre aussi les Pong).
 */
inline const std::string& canonical_genre(const std::string& g) {
    static const std::map<std::string, std::string> kMap = {
        // MAME vers FinalBurn Neo.
        {"Driving",       "Racing"},
        {"Platform",      "Platformer"},
        {"Fighter",       "Fighting / Versus"},
        {"Misc.",         "Misc"},
        {"Simulation",    "Simulator"},
        // FinalBurn Neo vers MAME, la ou c'est MAME qui nomme le mieux.
        {"Breakout",      "Ball & Paddle"},
    };
    const auto it = kMap.find(g);
    return it == kMap.end() ? g : it->second;
}

/* Un champ multiple venu du DAT : « Platformer, Action ».
 *
 * La virgule separe les valeurs, et pas la barre oblique : les libelles de
 * FBNeo en contiennent deja (« Shooter / Horizontal / Sh'mup »), tandis
 * qu'aucun ne contient de virgule. Chaque valeur compte pour elle, sinon
 * filtrer sur « Action » ne sortirait que les jeux portant exactement la
 * meme combinaison.
 */
inline std::vector<std::string> split_dat_values(const std::string& value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        std::string piece = value.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t a = piece.find_first_not_of(" \t");
        size_t b = piece.find_last_not_of(" \t");
        if (a != std::string::npos)
            out.push_back(canonical_genre(piece.substr(a, b - a + 1)));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

inline bool value_list_contains(const std::string& list, const std::string& wanted) {
    for (const auto& one : split_dat_values(list)) {
        if (one == wanted) return true;
    }
    return false;
}

struct Rom {
    std::string name;
    size_t size;
    std::string crc;  // CRC32 en hexadécimal
    // Attribut merge= du DAT : le nom sous lequel cette ROM existe deja dans
    // le set parent (romof), ou dans le BIOS. Vide pour une ROM propre au set.
    //
    // C'est ce qui distingue les trois styles de collection : en non-merged
    // le zip du jeu contient tout ; en split, une ROM merge= vit dans le zip
    // du parent et n'est PAS attendue ici ; en merged le clone n'a plus de zip
    // du tout. Sans cet attribut, seul le non-merged est comprehensible.
    std::string merge;

    bool is_inherited() const { return !merge.empty(); }
};

// Un disque dur, CD ou GD-ROM d'une machine MAME : un fichier CHD, range a
// cote des zips dans un dossier au nom du set (<racine>/<set>/<nom>.chd).
// Un CHD pese des centaines de Mo : on ne le relit jamais, on compare le SHA1
// que son en-tete declare a celui du DAT.
struct Disk {
    std::string name;   // sans l'extension .chd, comme le DAT l'ecrit
    std::string sha1;   // en minuscules
    std::string merge;  // attribut merge= : le meme disque, chez le parent
};

// Une machine MAME, reduite a ce que l'interface affiche. Les ROMs n'y sont
// pas : elles se redemandent a MAME set par set quand un ecran en a besoin.
struct MameMachine {
    std::string name;
    std::string description;
    std::string year;
    std::string manufacturer;
    std::string cloneof;
    std::string romof;
    std::string sourcefile;
    std::string driver_status;   // good | imperfect | preliminary
    bool is_bios       = false;
    bool is_device     = false;  // pas un jeu : une piece reutilisee par d'autres
    bool is_mechanical = false;  // flipper, machine a sous : non jouable ici
    bool runnable      = true;

    // Ce que `mame -listxml` donne deja et que le catalogue jetait. Sans eux
    // les filtres « Players » et « Orientation » ne montraient que du
    // FinalBurn Neo : faute de valeur, aucune machine MAME n'y entrait.
    int         players = 0;
    std::string orientation;     // « horizontal » / « vertical », vocabulaire des DAT
    std::string width;
    std::string height;
};

struct Game {
    // Quel emulateur fournit ce jeu : "fbneo" ou "mame". Persiste en base :
    // il fait partie de la cle de la table games, parce que `mslug` est un set
    // FinalBurn Neo ET un set MAME, tous deux en system 'Arcade'. La source
    // FBNeo le laisse a sa valeur par defaut, le catalogue MAME le renseigne.
    std::string emulator = "fbneo";

    std::string name;
    std::string description;
    std::string year;
    std::string manufacturer;
    std::string system;  // System type extracted from DAT header
    std::vector<Rom> roms;
    // Les CHD du set : seul le DAT « CHDs (merged) » de MAME en porte. Un set
    // qui n'a que des disques n'a aucune ROM, et inversement.
    std::vector<Disk> disks;
    // Les devices MAME dont la machine a besoin pour demarrer (<device_ref>) :
    // une puce son, un CPU de protection... chacun range dans son propre zip,
    // comme un set a part. Seuls ceux qui ont des ROMs a verifier sont gardes.
    // Vide pour FinalBurn Neo.
    std::vector<std::string> devices;
    std::string status = "missing";  // "available", "missing", "incorrect", "incomplete"
    
    // Video information
    std::string video_type = "";        // "raster", "vector", etc.
    std::string orientation = "";       // "horizontal", "vertical"
    std::string width = "";
    std::string height = "";
    std::string aspect_x = "";
    std::string aspect_y = "";
    
    // Driver information
    std::string driver_status = "";     // "good", "preliminary", "nodump"

    // Ce que le pilote FBNeo declare de lui-meme, ecrit dans le DAT par
    // notre fork (src/burner/dat.cpp). Absent des DAT d'amont : ces champs
    // restent vides, et les filtres correspondants ne s'affichent pas.
    //
    // `fb_hiscore` dit que FBNeo sait lire la table des scores de ce jeu.
    // C'est une question DIFFERENTE de « le service Bootcade classe-t-il ce
    // jeu », a laquelle repond HiscoreClient : 6 049 jeux contre 489.
    std::string genre = "";             // "Run 'n Gun", "Platformer, Action"
    std::string family = "";            // "Metal Slug", "Street Fighter"
    int         players = 0;            // 0 = non renseigne
    bool        fb_hiscore = false;
    
    // Additional information
    std::string comment = "";
    std::string cloneof = "";
    std::string romof = "";
    std::string sourcefile = "";
    // Attribut isbios="yes" du DAT : un set qui n'est pas un jeu mais le BIOS
    // dont dependent d'autres sets via romof (neogeo, gba, coleco...). Absent
    // de son zip, ce sont tous ces sets-la qui ne demarrent pas.
    bool        is_bios = false;
    std::string snapshot_path = "";  // Path to game screenshot/snapshot
    std::string dat_source = "";  // Source DAT file name
    // Raw <header><name> of the source DAT, e.g. "FinalBurn Neo - Arcade Games".
    // `system` is this string trimmed down to "Arcade"; the untrimmed form is what
    // the ROM manager names its output folders after, so a rebuilt tree drops
    // straight into a library laid out from the same DATs.
    std::string dat_header = "";

    // Play tracking & favourites
    bool        is_favorite     = false;
    std::string last_played     = "";   // ISO-8601 timestamp or empty
    int         play_count      = 0;
    int         play_time_secs  = 0;   // cumulative across every session
    // Kept alongside the cumulative total rather than derived from it: a sum
    // cannot answer "how long was my last go" or "what is my best run", and
    // recomputing either would need a per-session history we deliberately do
    // not keep : one row per game stays cheap on a 29 000-game catalogue.
    int         last_session_secs    = 0;
    int         longest_session_secs = 0;

    bool is_available() const { return status == "available"; }

    // ── Release-type classification ───────────────────────────────────────────
    // A game can carry several of these at once (a hack is usually also a clone,
    // a homebrew may be either), so these are independent predicates rather than
    // one enum. FBNeo DATs mark the nature of a set in its description, e.g.
    // "Airwolf (Hack, English)", "Jurl (48K) (HB)", "'99: The Last War (bootleg)";
    // parentage comes from the DAT's cloneof attribute.
    bool is_clone()     const { return !cloneof.empty(); }
    bool is_hack()      const { return desc_has("(hack"); }
    bool is_homebrew()  const { return desc_has("(hb)") || desc_has("(hb,") || desc_has("(hb "); }
    bool is_bootleg()   const { return desc_has("bootleg"); }
    bool is_prototype() const { return desc_has("(proto"); }

    // "Original" = an official commercial parent set. Homebrew is excluded even
    // though it is nobody's clone: it is an original work, but not an official
    // release, and lumping ~3.2k fan-made ZX Spectrum titles in with the
    // commercial catalogue is not what "original games only" is asked for.
    bool is_original() const {
        return !is_clone() && !is_hack() && !is_bootleg()
            && !is_prototype() && !is_homebrew();
    }

private:
    // Case-insensitive substring test against the description.
    bool desc_has(const std::string& needle) const {
        auto it = std::search(description.begin(), description.end(),
                              needle.begin(), needle.end(),
                              [](char a, char b) {
                                  return std::tolower((unsigned char)a) ==
                                         std::tolower((unsigned char)b);
                              });
        return it != description.end();
    }
};