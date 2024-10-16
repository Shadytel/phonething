#include "configuration.h"
#include <ctype.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum value_type {
    STRING,
    BOOLEAN,
    RANGE
};

static char *config_find_separator(char* ptr) {
    ptr = strchr(ptr, ' ');

    if (!ptr) {
        return NULL;
    }

    while (*ptr && (isspace(*ptr) || *ptr == '=' || *ptr == '>')) {
        ptr++;
    }

    return (*ptr) ? ptr : NULL;
}

static bool config_parse_range(const char *value, configuration_range_t *dest) {
    int valueLen;
    char *sepIndex;

    valueLen = strlen(value);

    if (valueLen == 0) {
        return false;
    }

    dest->min = atoi(value);

    sepIndex = strchr(value, '-');

    if (sepIndex != NULL) {
        dest->max = atoi(sepIndex + 1);
    } else {
        dest->max = dest->min;
    }

    return true;
}


void config_load_defaults(configuration_t* dest) {
    assert(dest != NULL);

    memset(dest, 0, sizeof(configuration_t));

    strncpy(dest->extensions.confbridge, "3*2004", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.confbridge, "3*2000", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.collectcall, "3*2001", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.normalivr, "3*2002", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.origtest, "3*2003", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.voicemail, "3*1114", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.emtanon1, "3*1337", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.voicemail, "21808", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.echotest, "71105", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.mtnschumer, "3*8888", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.shameshameshame, "3*7777", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.soundplayer, "71101", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.evansbot, "71102", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.newsfeed, "71103", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.projectupstage, "71104", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.callintercept, "17778033360", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.phreakspots, "3902", CONFIG_MAX_EXTEN);
    strncpy(dest->extensions.anac, "2622", CONFIG_MAX_EXTEN);

    strncpy(dest->adminivr, "418070", CONFIG_MAX_EXTEN);
    strncpy(dest->adminaddivr, "418080", CONFIG_MAX_EXTEN);
    strncpy(dest->defaultcpn, "2525350002", CONFIG_MAX_EXTEN);
    strncpy(dest->dialercpn, "2525350002", CONFIG_MAX_EXTEN);
    strncpy(dest->origtestcpn, "2525350002", CONFIG_MAX_EXTEN);
    strncpy(dest->dialersound, "", CONFIG_MAX_EXTEN);
    strncpy(dest->provisiondn, "3913", CONFIG_MAX_EXTEN);
    strncpy(dest->altprovisiondn, "3914", CONFIG_MAX_EXTEN);
    strncpy(dest->interceptdest, "3000", CONFIG_MAX_EXTEN);
    strncpy(dest->dialout_prefix, "15*", CONFIG_MAX_EXTEN);

    strncpy(dest->login, "71106", CONFIG_MAX_EXTEN);
    strncpy(dest->password, "5032970070", CONFIG_MAX_EXTEN);

    dest->cnetintercept = true;
}

static bool config_parse_line(const char* line, configuration_t* dest) {
    char* value;
    char *endp;
    size_t keyLen;
    size_t valLen;
    void* valueDest = NULL;
    enum value_type valueType = STRING;

    endp = strchr(line, ' ');

    if (!endp) {
        return false;
    }

    value = config_find_separator(endp);

    if (value == NULL) {
        return false;
    }

    keyLen = endp - line;

    if (!strncmp(line, "confbridge", keyLen)) {
        valueDest = &(dest->extensions.confbridge);
    } else if (!strncmp(line, "altconf", keyLen)) {
        valueDest = &(dest->extensions.altconf);
    } else if (!strncmp(line, "collectcall", keyLen)) {
        valueDest = &(dest->extensions.collectcall);
    } else if (!strncmp(line, "normalivr", keyLen)) {
        valueDest = &(dest->extensions.normalivr);
    } else if (!strncmp(line, "adminivr", keyLen)) {
        valueDest = &(dest->adminivr);
    } else if (!strncmp(line, "adminaddivr", keyLen)) {
        valueDest = &(dest->adminaddivr);
    } else if (!strncmp(line, "activation", keyLen)) {
        valueDest = &(dest->extensions.activation);
    } else if (!strncmp(line, "altactivation", keyLen)) {
        valueDest = &(dest->extensions.altactivation);
    } else if (!strncmp(line, "music", keyLen)) {
        valueDest = &(dest->extensions.music);
    } else if (!strncmp(line, "origtest", keyLen)) {
        valueDest = &(dest->extensions.origtest);
    } else if (!strncmp(line, "voicemail", keyLen)) {
        valueDest = &(dest->extensions.voicemail);
    } else if (!strncmp(line, "emtanon1", keyLen)) {
        valueDest = &(dest->extensions.emtanon1);
    } else if (!strncmp(line, "emtanon2", keyLen)) {
        valueDest = &(dest->extensions.emtanon2);
    } else if (!strncmp(line, "echotest", keyLen)) {
        valueDest = &(dest->extensions.echotest);
    } else if (!strncmp(line, "mtnschumer", keyLen)) {
        valueDest = &(dest->extensions.mtnschumer);
    } else if (!strncmp(line, "shameshameshame", keyLen)) {
        valueDest = &(dest->extensions.shameshameshame);
    } else if (!strncmp(line, "soundplayer", keyLen)) {
        valueDest = &(dest->extensions.soundplayer);
    } else if (!strncmp(line, "evansbot", keyLen)) {
        valueDest = &(dest->extensions.evansbot);
    } else if (!strncmp(line, "newsfeed", keyLen)) {
        valueDest = &(dest->extensions.newsfeed);
    } else if (!strncmp(line, "phreakspots", keyLen)) {
        valueDest = &(dest->extensions.phreakspots);
    } else if (!strncmp(line, "projectupstage", keyLen)) {
        valueDest = &(dest->extensions.projectupstage);
    } else if (!strncmp(line, "telechallenge", keyLen)) {
        valueDest = &(dest->extensions.telechallenge);
    } else if (!strncmp(line, "callintercept", keyLen)) {
        valueDest = &(dest->extensions.callintercept);
    } else if (!strncmp(line, "anac", keyLen)) {
        valueDest = &(dest->extensions.anac);
    } else if (!strncmp(line, "defaultcpn", keyLen)) {
        valueDest = &(dest->defaultcpn);
    } else if (!strncmp(line, "dialercpn", keyLen)) {
        valueDest = &(dest->dialercpn);
    } else if (!strncmp(line, "dialersound", keyLen)) {
        valueDest = &(dest->dialersound);
    } else if (!strncmp(line, "origtestcpn", keyLen)) {
        valueDest = &(dest->origtestcpn);
    } else if (!strncmp(line, "dialout_prefix", keyLen)) {
        valueDest = &(dest->dialout_prefix);
    } else if (!strncmp(line, "interceptdest", keyLen)) {
        valueDest = &(dest->interceptdest);
    } else if (!strncmp(line, "login", keyLen)) {
        valueDest = &(dest->login);
    } else if (!strncmp(line, "password", keyLen)) {
        valueDest = &(dest->password);
    } else if (!strncmp(line, "provisiondn", keyLen)) {
        valueDest = &(dest->provisiondn);
    } else if (!strncmp(line, "altprovisiondn", keyLen)) {
        valueDest = &(dest->altprovisiondn);
    } else if (!strncmp(line, "cnetintercept", keyLen)) {
        valueDest = &(dest->cnetintercept);
        valueType = BOOLEAN;
    } else if (!strncmp(line, "analog", keyLen)) {
        valueDest = &(dest->analog_channels);
        valueType = RANGE;
    } else if (!strncmp(line, "isdn", keyLen)) {
        valueDest = &(dest->isdn_channels);
        valueType = RANGE;
    }

    if (valueDest == NULL) {
        return false;
    }

    if (valueType == STRING) {
        valLen = strlen(value);

        // Trim off trailing whitespace
        while (isspace(value[valLen - 1])) {
            value[valLen - 1] = 0;
            valLen--;
        }

        strncpy((char *) valueDest, value, CONFIG_MAX_EXTEN);
    } else if (valueType == BOOLEAN) {
        *((bool*)valueDest) = (value[0] == 'y' || value[0] == 'Y' || value[0] == '1');
    } else if (valueType == RANGE) {
        return config_parse_range(value, (configuration_range_t *) valueDest);
    }

    return true;
}

bool config_parse_file(const char* path, configuration_t* dest) {
    FILE *fp;
    int lineNum;
    char line[256];

    assert(path != NULL);
    assert(dest != NULL);

    fp = fopen(path, "r");

    if (!fp) {
        return false;
    }

    memset(dest, 0, sizeof(configuration_t));

    lineNum = 0;

    while (fgets(line, 256, fp)) {
        lineNum++;

        if (!line[0] || line[0] == '#' || line[0] == '\n') {
            continue;
        }

        if (!config_parse_line(line, dest)) {
            fprintf(stderr, "warning: syntax error in %s on or near line %d - good luck\n", path, lineNum);
        }
    }


    fclose(fp);

    return true;
}
/*
    char defaultcpn[CONFIG_MAX_EXTEN];
    char dialercpn[CONFIG_MAX_EXTEN];
    char origtestcpn[CONFIG_MAX_EXTEN];
    char dialersound[CONFIG_MAX_EXTEN];

    char interceptdest[CONFIG_MAX_EXTEN];

    char login[CONFIG_MAX_EXTEN];
    char password[CONFIG_MAX_EXTEN];

    bool cnetintercept;

    configuration_range_t analog_channels;
    configuration_range_t isdn_channels;
*/
void config_dump(const configuration_t *conf) {
    assert(conf != NULL);

    printf("extensions:\n");
    printf("\tconfbridge: %s\n", conf->extensions.confbridge);
    printf("\tcollectcall: %s\n", conf->extensions.collectcall);
    printf("\tnormalivr: %s\n", conf->extensions.normalivr);
    printf("\torigtest: %s\n", conf->extensions.origtest);
    printf("\tvoicemail: %s\n", conf->extensions.voicemail);
    printf("\temtanon1: %s\n", conf->extensions.emtanon1);
    printf("\temtanon2: %s\n", conf->extensions.emtanon2);
    printf("\techotest: %s\n", conf->extensions.echotest);
    printf("\tmtnschumer: %s\n", conf->extensions.mtnschumer);
    printf("\tshameshameshame: %s\n", conf->extensions.shameshameshame);
    printf("\tevansbot: %s\n", conf->extensions.evansbot);
    printf("\tsoundplayer: %s\n", conf->extensions.soundplayer);
    printf("\tnewsfeed: %s\n", conf->extensions.newsfeed);
    printf("\tprojectupstage: %s\n", conf->extensions.projectupstage);
    printf("\tcallintercept: %s\n", conf->extensions.callintercept);
    printf("\tphreakspots: %s\n", conf->extensions.phreakspots);
    printf("defaultcpn: %s\n", conf->defaultcpn);
    printf("dialercpn: %s\n", conf->dialercpn);
    printf("origtestcpn: %s\n", conf->origtestcpn);
    printf("dialersound: %s\n", conf->dialersound);
    printf("provisiondn: %s\n", conf->provisiondn);
    printf("altprovisiondn: %s\n", conf->altprovisiondn);
    printf("dialout_prefix: %s\n", conf->dialout_prefix);

    printf("interceptdest: %s\n", conf->interceptdest);

    printf("login: %s\n", conf->login);
    printf("password: %s\n", conf->password);

    printf("cnetintercept: %s\n", conf->cnetintercept ? "true" : "false");

    printf("analog_channels: %d -> %d\n", conf->analog_channels.min, conf->analog_channels.max);
    printf("isdn_channels: %d -> %d\n", conf->isdn_channels.min, conf->isdn_channels.max);
}
