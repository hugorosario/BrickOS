#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include "../mainui/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PATH_BYTES 1024
#define SYSTEM_JSON_MAX (256 * 1024)

// Every text, in English and in European Portuguese. The confirmation screen names what goes,
// what stays and the restart, as the spec lists them.
static const char *const texts[UNINSTALL_TEXT_COUNT][2] = {
    [UNINSTALL_TEXT_TITLE] = {"Uninstall BrickOS", "Desinstalar o BrickOS"},
    [UNINSTALL_TEXT_NOTICE] = {"The last uninstall did not finish: %s", "A última desinstalação não terminou: %s"},
    [UNINSTALL_TEXT_NOTICE_NO_REASON] = {"The last uninstall did not finish.", "A última desinstalação não terminou."},
    [UNINSTALL_TEXT_REMOVES] = {"This removes", "Isto remove"},
    [UNINSTALL_TEXT_REMOVES_COMPONENTS] = {
        "The BrickOS MainUI, the input daemon, the OSD, the LED daemon, the in-game menu and GameSwitcher",
        "A MainUI do BrickOS, o daemon de entrada, o OSD, o daemon dos LEDs, o menu no jogo e o GameSwitcher"},
    [UNINSTALL_TEXT_REMOVES_FOLDER] = {
        "The whole BrickOS folder, with GameSwitcher's checkpoints and every BrickOS setting",
        "Toda a pasta BrickOS, com os checkpoints do GameSwitcher e todas as definições do BrickOS"},
    [UNINSTALL_TEXT_KEEPS] = {"This stays", "Isto fica"},
    [UNINSTALL_TEXT_KEEPS_DATA] = {"Games, saves, Favorites, Recent, other apps and themes",
                                   "Jogos, gravações de jogo, Favoritos, Recentes, outras aplicações e temas"},
    [UNINSTALL_TEXT_KEEPS_IMAGES] = {"The bootlogo and splash, as last applied", "O bootlogo e o splash, tal como aplicados"},
    [UNINSTALL_TEXT_RESTART] = {"The Brick restarts, and BrickOS is removed while it starts.",
                                "O Brick reinicia e o BrickOS é removido durante o arranque."},
    [UNINSTALL_TEXT_CANCEL] = {"Cancel", "Cancelar"},
    [UNINSTALL_TEXT_UNINSTALL] = {"Uninstall", "Desinstalar"},
    [UNINSTALL_TEXT_HINT] = {"LEFT/RIGHT  Choose     A  Select     B  Cancel",
                             "ESQ./DIR.  Escolher     A  Selecionar     B  Cancelar"},
    [UNINSTALL_TEXT_ERROR_TITLE] = {"Could not uninstall now", "Não foi possível desinstalar agora"},
    [UNINSTALL_TEXT_ERROR_SCRIPT_MISSING] = {"The app's uninstall script is missing.",
                                             "Falta o script de desinstalação da aplicação."},
    [UNINSTALL_TEXT_ERROR_CARD_FULL] = {"The SD card is full.", "O cartão SD está cheio."},
    [UNINSTALL_TEXT_ERROR_READ_ONLY] = {"The SD card cannot be written.", "Não é possível escrever no cartão SD."},
    [UNINSTALL_TEXT_ERROR_WRITE] = {"The uninstall start script could not be written.",
                                    "Não foi possível escrever o script de arranque da desinstalação."},
    [UNINSTALL_TEXT_ERROR_RESTART] = {"The Brick could not restart.", "Não foi possível reiniciar o Brick."},
    [UNINSTALL_TEXT_ERROR_UNCHANGED] = {"Nothing was changed.", "Nada foi alterado."},
    [UNINSTALL_TEXT_ERROR_NEXT_START] = {"BrickOS will be removed the next time the Brick starts.",
                                         "O BrickOS será removido no próximo arranque do Brick."},
    [UNINSTALL_TEXT_BACK] = {"Back to MainUI", "Voltar à MainUI"},
    [UNINSTALL_TEXT_RESTARTING] = {"Restarting", "A reiniciar"},
    [UNINSTALL_TEXT_RESTARTING_DETAIL] = {"BrickOS is removed while the Brick starts. This can take a few minutes.",
                                          "O BrickOS é removido durante o arranque do Brick. Pode demorar alguns minutos."},
};

void uninstall_model_init(struct uninstall_model *m, enum uninstall_language language) {
    memset(m, 0, sizeof(*m));
    m->screen = UNINSTALL_SCREEN_CONFIRM;
    m->focus = UNINSTALL_CHOICE_CANCEL;
    m->language = language;
}

enum uninstall_language uninstall_language_of(const char *language_file) {
    if (language_file && strncasecmp(language_file, "pt", 2) == 0 &&
        (language_file[2] == '.' || language_file[2] == '_' || language_file[2] == '-'))
        return UNINSTALL_PORTUGUESE;
    return UNINSTALL_ENGLISH;
}

// Reads a whole file below root, up to max bytes; NULL when it is missing, empty or larger.
static char *read_file(const char *root, const char *path, size_t max, size_t *length) {
    char full[PATH_BYTES];
    if (snprintf(full, sizeof(full), "%s%s", root ? root : "", path) >= (int)sizeof(full)) return NULL;
    FILE *file = fopen(full, "rb");
    if (!file) return NULL;
    char *text = malloc(max + 1);
    size_t n = text ? fread(text, 1, max + 1, file) : 0;
    fclose(file);
    if (!text || n == 0 || n > max) { free(text); return NULL; }
    text[n] = 0;
    *length = n;
    return text;
}

struct language_search { char *out; size_t size; int found; };

static int keep_language(void *context, const struct mainui_json_member *member) {
    struct language_search *search = context;
    // cJSON, which the firmware reads system.json with, finds keys without case.
    if (member->type != MAINUI_JSON_STRING || strcasecmp(member->key, "language") != 0) return 0;
    snprintf(search->out, search->size, "%s", member->text);
    search->found = 1;
    return -1;
}

int uninstall_read_language(const char *root, char *out, size_t size) {
    size_t length = 0;
    if (size) out[0] = 0;
    char *text = read_file(root, UNINSTALL_SYSTEM_JSON, SYSTEM_JSON_MAX, &length);
    if (!text) return -1;
    struct language_search search = {out, size, 0};
    mainui_json_each(text, length, keep_language, &search);
    free(text);
    if (!search.found && size) out[0] = 0;
    return search.found ? 0 : -1;
}

int uninstall_read_result(const char *root, char *reason, size_t size) {
    size_t length = 0;
    int failed = 0;
    if (size) reason[0] = 0;
    char *text = read_file(root, UNINSTALL_RESULT_FILE, 64 * 1024, &length);
    if (!text) return 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[--n] = 0;
        if (strcmp(line, "result=failed") == 0) failed = 1;
        else if (strncmp(line, "reason=", 7) == 0) snprintf(reason, size, "%s", line + 7);
    }
    free(text);
    if (!failed && size) reason[0] = 0;
    return failed;
}

void uninstall_model_load(struct uninstall_model *m, const char *root) {
    char language[64];
    uninstall_read_language(root, language, sizeof(language));
    uninstall_model_init(m, uninstall_language_of(language));
    m->previous_failed = uninstall_read_result(root, m->reason, sizeof(m->reason));
}

enum uninstall_action uninstall_model_key(struct uninstall_model *m, enum uninstall_key key) {
    switch (m->screen) {
    case UNINSTALL_SCREEN_CONFIRM:
        switch (key) {
        case UNINSTALL_KEY_LEFT: m->focus = UNINSTALL_CHOICE_CANCEL; return UNINSTALL_ACTION_NONE;
        case UNINSTALL_KEY_RIGHT: m->focus = UNINSTALL_CHOICE_UNINSTALL; return UNINSTALL_ACTION_NONE;
        case UNINSTALL_KEY_A:
            return m->focus == UNINSTALL_CHOICE_UNINSTALL ? UNINSTALL_ACTION_UNINSTALL : UNINSTALL_ACTION_EXIT;
        case UNINSTALL_KEY_B: return UNINSTALL_ACTION_EXIT;
        }
        break;
    case UNINSTALL_SCREEN_ERROR:
        return key == UNINSTALL_KEY_A || key == UNINSTALL_KEY_B ? UNINSTALL_ACTION_EXIT : UNINSTALL_ACTION_NONE;
    case UNINSTALL_SCREEN_RESTARTING:
        break;
    }
    return UNINSTALL_ACTION_NONE;
}

void uninstall_model_staged(struct uninstall_model *m) {
    m->screen = UNINSTALL_SCREEN_RESTARTING;
    m->error = UNINSTALL_ERROR_NONE;
}

void uninstall_model_failed(struct uninstall_model *m, enum uninstall_error error) {
    m->screen = UNINSTALL_SCREEN_ERROR;
    m->error = error == UNINSTALL_ERROR_NONE ? UNINSTALL_ERROR_WRITE : error;
}

const char *uninstall_text(const struct uninstall_model *m, enum uninstall_text text) {
    if ((unsigned)text >= UNINSTALL_TEXT_COUNT) return "";
    return texts[text][m->language == UNINSTALL_PORTUGUESE];
}

void uninstall_notice(const struct uninstall_model *m, char *out, size_t size) {
    if (!size) return;
    out[0] = 0;
    if (!m->previous_failed) return;
    if (!m->reason[0]) {
        snprintf(out, size, "%s", uninstall_text(m, UNINSTALL_TEXT_NOTICE_NO_REASON));
        return;
    }
    const char *format = uninstall_text(m, UNINSTALL_TEXT_NOTICE), *mark = strstr(format, "%s");
    snprintf(out, size, "%.*s%s%s", (int)(mark - format), format, m->reason, mark + 2);
}

const char *uninstall_error_text(const struct uninstall_model *m) {
    switch (m->error) {
    case UNINSTALL_ERROR_SCRIPT_MISSING: return uninstall_text(m, UNINSTALL_TEXT_ERROR_SCRIPT_MISSING);
    case UNINSTALL_ERROR_CARD_FULL: return uninstall_text(m, UNINSTALL_TEXT_ERROR_CARD_FULL);
    case UNINSTALL_ERROR_READ_ONLY: return uninstall_text(m, UNINSTALL_TEXT_ERROR_READ_ONLY);
    case UNINSTALL_ERROR_RESTART: return uninstall_text(m, UNINSTALL_TEXT_ERROR_RESTART);
    case UNINSTALL_ERROR_NONE:
    case UNINSTALL_ERROR_WRITE: break;
    }
    return uninstall_text(m, UNINSTALL_TEXT_ERROR_WRITE);
}
