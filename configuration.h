#ifndef _CONFIGURATION_H_INCLUDED
#define _CONFIGURATION_H_INCLUDED

#include <stdbool.h>

#define CONFIG_MAX_EXTEN 24

typedef struct {
        int min;
        int max;
} configuration_range_t;

typedef struct {
        struct {
                char confbridge[CONFIG_MAX_EXTEN];
                char collectcall[CONFIG_MAX_EXTEN];
                char normalivr[CONFIG_MAX_EXTEN];
                char origtest[CONFIG_MAX_EXTEN];
                char voicemail[CONFIG_MAX_EXTEN];
                char emtanon1[CONFIG_MAX_EXTEN];
                char emtanon2[CONFIG_MAX_EXTEN];
                char anac[CONFIG_MAX_EXTEN];
                char echotest[CONFIG_MAX_EXTEN];
                char mtnschumer[CONFIG_MAX_EXTEN];
                char shameshameshame[CONFIG_MAX_EXTEN];
                char evansbot[CONFIG_MAX_EXTEN];
                char soundplayer[CONFIG_MAX_EXTEN];
                char newsfeed[CONFIG_MAX_EXTEN];
                char phreakspots[CONFIG_MAX_EXTEN];
                char activation[CONFIG_MAX_EXTEN];
                char altactivation[CONFIG_MAX_EXTEN];
                char music[CONFIG_MAX_EXTEN];
                char altconf[CONFIG_MAX_EXTEN];
                char projectupstage[CONFIG_MAX_EXTEN];

                char callintercept[CONFIG_MAX_EXTEN];
        } extensions;

        char defaultcpn[CONFIG_MAX_EXTEN];
        char dialercpn[CONFIG_MAX_EXTEN];
        char origtestcpn[CONFIG_MAX_EXTEN];
        char dialersound[CONFIG_MAX_EXTEN];
        char adminivr[CONFIG_MAX_EXTEN];
        char adminaddivr[CONFIG_MAX_EXTEN];
        char provisiondn[CONFIG_MAX_EXTEN];
        char altprovisiondn[CONFIG_MAX_EXTEN];
        char interceptdest[CONFIG_MAX_EXTEN];

        char login[CONFIG_MAX_EXTEN];
        char password[CONFIG_MAX_EXTEN];

        bool cnetintercept;

        configuration_range_t analog_channels;
        configuration_range_t isdn_channels;
} configuration_t;

void config_load_defaults(configuration_t* dest);
bool config_parse_file(const char* path, configuration_t* dest);
void config_dump(const configuration_t* conf);

#endif

