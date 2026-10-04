// src/FbneoVideo.h : les options d'affichage du frontend SDL2 de FinalBurn Neo.
#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

/* ── Ce que le binaire sait faire, et ce que le joueur en a choisi ──────
 *
 * Les filtres SoftFX, les motifs de masque RGB et les backends de rendu ne
 * sont PAS ecrits ici : ils dependent du build (quatre filtres demandent un
 * build assembleur x86, les backends dependent de la SDL de la machine).
 * Bootcade les demande a FinalBurn Neo lui-meme par `fbneo -list-video-json`
 * et n'affiche que ce que ce binaire-la accepte.
 *
 * Un binaire qui ne connait pas -list-video-json (un build plus ancien) ne
 * recoit AUCUNE de ces options : un drapeau inconnu pourrait etre pris pour
 * le nom du jeu, et le lancement echouerait sur un reglage d'affichage.
 */
namespace FbneoVideo {

struct Filter {
    int              index = 0;
    std::string      name;
    int              zoom = 1;
    bool             available = false;
    std::vector<int> depths;
};

struct Mask {
    int         index = 0;
    std::string name;
};

struct Range {
    int min = 0;
    int max = 0;
    int def = 0;
    bool contains(int v) const { return v >= min && v <= max; }
};

struct Capabilities {
    bool                     ok = false;   // le binaire a repondu a -list-video-json
    std::vector<Filter>      softfx;
    std::vector<Mask>        rgbmask;
    std::vector<std::string> renderers;
    Range                    softfx_range{-1, -1, -1};
    Range                    rgbmask_range{0, 0, 0};
    Range                    internalres_range{1, 1, 1};
    Range                    scanintensity_range{0, 255, 191};
    std::string              renderer_default;

    const Filter* filter(int index) const;
    bool          has_mask(int index) const;
    bool          has_renderer(const std::string& name) const;
};

// Lit la sortie de -list-video-json. Rend ok=false si elle n'en est pas une.
Capabilities parse(const std::string& text);

/* Interroge le binaire, une fois par chemin et par date de modification :
 * un FBNeo remplace par une mise a jour est donc interroge a nouveau.
 * Bloquant, mais tres court (le binaire repond avant toute initialisation).
 * Sur de multiples fils : protege par un verrou. */
Capabilities probe(const std::string& exe);

/* Les choix du joueur. Un interrupteur et une valeur par option : eteindre
 * un filtre ne fait pas oublier lequel on avait choisi. */
struct Options {
    bool        softfx_on = false;
    int         softfx = -1;
    bool        scanlines = false;
    int         scanintensity = 191;
    bool        rgbmask_on = false;
    int         rgbmask = 0;
    bool        stretch = false;
    bool        internalres_on = false;
    int         internalres = 1;
    bool        renderer_on = false;
    std::string renderer;

    void load(const nlohmann::json& o);
    void save(nlohmann::json& o) const;

    /* Les arguments a passer au lancement : seulement ceux qui different du
     * defaut annonce par le binaire, et seulement les valeurs qu'il accepte.
     * Vide si le binaire n'a pas repondu a la sonde. */
    std::vector<std::string> launch_args(const Capabilities& caps) const;

    // Combien d'options changent reellement l'image (resume de la carte).
    int active_count(const Capabilities& caps) const;
};

}  // namespace FbneoVideo
