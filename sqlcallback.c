#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <fcntl.h>
#include <srllib.h>
#include <dxxxlib.h>
#include <sys/stat.h>
#include "answer.h"
#include "configuration.h"
#include "sql.h"


// For stat()
struct stat sts;
// Let's declare some functions!
int play(short channum, int filedesc, int format, unsigned long offset, char options);
char playmulti(short channum, char numprompts, uint8_t format, int sounddescriptor[numprompts]);
void file_error(short channum, char *file_name);
void disp_msg(char *stringp);
void disp_err(int chnum, int chfd, int state);
void disp_msgf(const char *fmt, ...);
short idle_trunkhunt( short channum, short low, short high, bool sigreset );
char makecall(short channum, char *destination, char *callingnum, char rec);

int admin_cb_invalid(short channum) {
    dxinfox[ channum ].state = ST_ADMINACTE;
    if (play(channum, invalidfd, 0, 0, 0) != 0) {
        dxinfox[ channum ].state = ST_ERROR;
        play(channum, errorfd, 0, 0, 0);
        return -1;
    }
    return 0;
}

int userpass_cb_invalid(short channum) {
    memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
    dxinfox[ channum ].state = ST_ACTIVATIONF;
    dxinfox[ channum ].msg_fd = open("sounds/activation/activation_invalid_extpass.pcm", O_RDONLY);
    if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
        file_error(channum, "sounds/activation/activation_invalid_extpass.pcm");
        return -1;
    }
    return 0;
}

int callednum_cb(void * chanptr, int argc, char **argv, char **coldata) {
    int channum = *((int *) chanptr);
    int count = atoi(argv[0]);
    if (count == 0) {
        // This number hasn't been called before - let's add it and, well, call it.
        connchan[ channum ] = idle_trunkhunt( channum, 1, maxchans, false );
        if (connchan[ channum ] == -1) {
            // Do we need this, or will the SQL error handler catch it?
            disp_msg("ERROR: Couldn't make test call!");
            dxinfox[channum].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
            return -1;
        }
        dxinfox[connchan[channum]].state = ST_OUTCALLTEST;
        snprintf(filetmp2[channum], MAXMSG, "%s1%s", config.dialout_prefix, filetmp[channum]);
        makecall(connchan[channum], filetmp2[channum], "7753390466", FALSE);
        // Add the number to the database here.
        char query[66];
        char * err_msg;
        snprintf( query, 65, "INSERT INTO called_numbers (phone_number) VALUES ('%s');", filetmp[channum]);
        if (sqlite3_exec( tc_blacklist, query, NULL, NULL, &err_msg) != SQLITE_OK) {
            disp_msgf("SQL INSERT ERROR: %s", err_msg);
            memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
            memset(filetmp2[channum], 0x00, sizeof(filetmp[channum]));
            dxinfox[channum].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            return -1;
        }
        dxinfox[ channum ].msg_fd = open("sounds/thanks.pcm", O_RDONLY);
        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
            file_error(channum, "sounds/thanks.pcm");
            memset(filetmp2[channum], 0x00, sizeof(filetmp[channum]));
            memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
            return -1;
        }
        connchan[ channum ] = 0; // Zero this out so it doesn't get mistakenly used elsewhere
        // Zero out the array for now, since we're not actually doing anything with it yet.
        memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
        memset(filetmp2[channum], 0x00, sizeof(filetmp[channum]));
    }
    else {
        memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
        dxinfox[ channum ].state = ST_TC24CALLDB;
        dxinfox[ channum ].msg_fd = open("sounds/tc24/callout_num_blocked.pcm", O_RDONLY);
        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
            file_error(channum, "sounds/tc24/callout_num_blocked.pcm");
            return -1;
        }
        return 0;
    }

    return 0;
}

int npanxx_cb(void * chanptr, int argc, char **argv, char **coldata) {
    int channum = *((int *) chanptr);
    int count = atoi(argv[0]);
    if (count == 0) {
        // NPA-NXX isn't blocked. Let's do a final test for the number itself.
        char query[73];
        char * err_msg;
        snprintf(query, 72, "SELECT COUNT(*) FROM called_numbers where phone_number = '%s';", filetmp[channum]);
        if (sqlite3_exec( tc_blacklist, query, callednum_cb, (void*) &channum, &err_msg) != SQLITE_OK) {
            disp_msgf("SQL SELECT ERROR: %s", err_msg);
            dxinfox[channum].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            return -1;
        }
    }
    else {
        memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
        dxinfox[ channum ].state = ST_TC24CALLDB;
        dxinfox[ channum ].msg_fd = open("sounds/tc24/callout_num_blocked.pcm", O_RDONLY);
        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
            file_error(channum, "sounds/tc24/callout_num_blocked.pcm");
            return -1;
        }
        return 0;
    }

    return 0;

}

int npa_cb(void * chanptr, int argc, char **argv, char **coldata) {
    int channum = *((int *) chanptr);
    int count = atoi(argv[0]);
    if (count == 0) {
        // Area code isn't blocked. Let's check the NXX.
        char * err_msg;
        char query[101];
        char npa[4];
        char nxx[4];
        // TO DO: Put trunk code offset into the next lines, i.e., filetmp[channum]+strlen(trunkcode)
        snprintf(npa, 4, "%s", filetmp[channum] );
        snprintf(nxx, 4, "%s", filetmp[channum] + 3);
        npa[3] = 0x00; // Insert null terminator
        nxx[3] = 0x00;
        disp_msgf("DEBUG: About to send query with NPA %s and NXX %s", npa, nxx);
        snprintf(query, 100, "SELECT COUNT(*) FROM exchanges WHERE (area_code IS NULL OR area_code = '%s') AND exchange = '%s';", npa, nxx);
        if (sqlite3_exec( tc_blacklist, query, npanxx_cb, (void*) &channum, &err_msg) != SQLITE_OK) {
            disp_msgf("SQL SELECT ERROR: %s", err_msg);
            dxinfox[channum].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            return -1;
        }
    }
    else {
        memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
        dxinfox[ channum ].state = ST_TC24CALLDB;
        dxinfox[ channum ].msg_fd = open("sounds/tc24/callout_num_blocked.pcm", O_RDONLY);
        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
            file_error(channum, "sounds/tc24/callout_num_blocked.pcm");
            return -1;
        }
        return 0;
    }
    return 0;
}

int userpass_cb(void * chanptr, int argc, char **argv, char **coldata)
{
    int channum = *((int *) chanptr);
    if (argc != 3) {
        disp_msg("SQL query gave wrong number of columns!");
        return userpass_cb_invalid(channum);
    }
    else {
        // Check username
        if (strcmp(filetmp[channum], argv[0]) == 0) {
            // Check password
            if (strcmp(argv[1], dxinfox[ channum ].digbuf.dg_value) == 0) {
                if (argv[2][0] == 0x30) {
                    // Everything's okay! Update the parameter in the database to indicate the account's already been registered
                    // TO DO: SQL query to set indicator in database for phone already registered
                char query[57];
                char * err_msg;
                snprintf( query, 56, "UPDATE USERS SET REGISTERED = 1 WHERE EXTENSION = %s;", filetmp[channum]);
                if (sqlite3_exec( activationdb, query, NULL, NULL, &err_msg) != SQLITE_OK) {
                    disp_msgf("SQL UPDATE ERROR: %s", err_msg);
                    memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return -1;
                }

                    dxinfox[channum].state = ST_ACTIVATION4;
                    if (strcmp(config.extensions.activation, isdninfo[ channum ].dnis) == 0) {
                        sprintf(filetmp2[channum], "%s", config.provisiondn);
                    }

                    else {
                        sprintf(filetmp2[channum], "%s", config.altprovisiondn);
                    }
                    if (stat(filetmp2[channum], &sts) == -1) {
                        mkdir(filetmp2[channum], 0666);    // Create the directory if it's not there
                    }
                    sprintf(filetmp2[channum], "%s/%s.ord", filetmp2[channum], isdninfo[ channum ].cpn);
                    disp_msgf("DEBUG: order file is %s", filetmp2[channum]);
                    resumefile[ channum ] = fopen(filetmp2[ channum ], "w");
                    if (resumefile[channum] == NULL) {
                        disp_msgf("ERROR: Couldn't create order file for subscriber %s!", isdninfo[ channum ].cpn);
                        memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                        memset(filetmp2[channum], 0x00, sizeof(filetmp2[channum]));
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                        return -1;
                    }
                    if (strcmp(config.extensions.activation, isdninfo[ channum ].dnis) == 0) {
                        disp_msg("DEBUG: This is a lower camp extension");
                        fprintf(resumefile[ channum ], "%s,%s,%s,%s,", isdninfo[ channum ].cpn, filetmp[channum], config.provisiondn, config.altprovisiondn);
                    }
                    else {
                        fprintf(resumefile[ channum ], "%s,%s,%s,%s,", isdninfo[ channum ].cpn, filetmp[channum], config.altprovisiondn, config.provisiondn);
                    }
                    fclose(resumefile[channum]);
                    memset(filetmp[channum], 0x00, sizeof(filetmp[channum])); // We're done with this temporary nonsense, get rid of it.
                    memset(filetmp2[channum], 0x00, sizeof(filetmp2[channum])); // We're done with this temporary nonsense, get rid of it.
                    multiplay[channum][0] = open("sounds/activation/activation_allset.pcm", O_RDONLY);
                    sprintf(dxinfox[ channum ].msg_name, "sounds/activation/activation_tyshadytel%d.pcm", random_at_most(2)+1);
                    multiplay[channum][1] = open(dxinfox[channum].msg_name, O_RDONLY);
                    if (playmulti(channum, 2, 0, multiplay[channum]) == -1) {
                        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                        disp_msg("ERROR: Couldn't play back activation recording!");
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                        return -1;
                    }
                    return 0;
                }

                else {
                    // Someone's already provisioned the account. Let the caller know and boot them back to the main menu.
                    memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                    dxinfox[ channum ].state = ST_ACTIVATIONF;
                    dxinfox[ channum ].msg_fd = open("sounds/activation/activation_alreadyactive.pcm", O_RDONLY);
                    if (play(channum, dxinfox[channum].msg_fd, 1, 0, 1) != 0) {
                        file_error(channum, "sounds/activation/activation_alreadyactive.pcm");
                        return -1;
                    }
                    return 0;
                }
            }
            else return userpass_cb_invalid(channum);
        }
        else return userpass_cb_invalid(channum);

    }

}

int count_cb(void * chanptr, int argc, char **argv, char **coldata)
{
    int channum = *((int *) chanptr);
    int count = atoi(argv[0]);
    if (count == 0) {
        // SQL database returned zero entries for that query.
        // Someone's pulling our leg here; the username clearly doesn't exist.
        return userpass_cb_invalid(channum);
    }
    else if (count == 1) {
        char query[45];
        char * err_msg;
        snprintf( query, 44, "SELECT * FROM USERS WHERE EXTENSION = %s;", filetmp[channum]);
        if (sqlite3_exec( activationdb, query, userpass_cb, (void*) &channum, &err_msg) != SQLITE_OK) {
            disp_msgf("SQL SELECT ERROR: %s", err_msg);
            memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
            dxinfox[channum].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            return -1;
        }
        else return 0;
    }
    else {
        // There's more than one of these. What happened exactly?
        char query[53];
        char * err_msg;
        // Uhhhm, let's just... yeah.
        snprintf( query, 52, "SELECT * FROM USERS WHERE EXTENSION = %s LIMIT 1;", filetmp[channum]);
        if (sqlite3_exec( activationdb, query, userpass_cb, (void*) &channum, &err_msg) != SQLITE_OK) {
            disp_msgf("SQL SELECT ERROR: %s", err_msg);
            memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
            dxinfox[channum].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            return -1;
        }
        else return 0;
    }
}

int admin_cb(void * chanptr, int argc, char **argv, char **coldata)
{
    int channum = *((int *) chanptr);
    if (argc != 3) {
        disp_msg("SQL query gave wrong number of columns!");
        return admin_cb_invalid(channum);
    }
    else {
        // Copy queried extension to not the digit buffer

        strcpy(filetmp[channum], dxinfox[ channum ].digbuf.dg_value);
        if (argv[2][0] == 0x30) {
            // The extension's not yet been activated
            multiplay[channum][0] = open("sounds/activation/activationivr_admin_extnotactivated.pcm", O_RDONLY);
        }
        else {
            // Extension already activated
            multiplay[channum][0] = open("sounds/activation/activationivr_admin_extactivated.pcm", O_RDONLY);
        }
        multiplay[channum][1] = open("sounds/activation/activationivr_adminmenu.pcm", O_RDONLY);
        dxinfox[ channum ].state = ST_ADMINACT2;
        if (playmulti(channum, 2, 128, multiplay[channum]) == -1) {
            disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
            disp_msg("ERROR: Couldn't play back admin menu recording!");
            dxinfox[ channum ].state = ST_ERROR;
            play(channum, errorfd, 0, 0, 1);
            return -1;
        }
        return 0;
    }

}

int adminadd_cb(void * chanptr, int argc, char **argv, char **coldata)
{
    int channum = *((int *) chanptr);

    int count = atoi(argv[0]);
    if (count == 0) {
        // No extension has been previously defined - good; we can add it.
        strcpy(filetmp[channum], dxinfox[ channum ].digbuf.dg_value);
        dxinfox[ channum ].state = ST_ADMINADD2;
        dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_addmenu.pcm", O_RDONLY);
        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
            file_error(channum, "sounds/activation/activationivr_admin_addmenu.pcm");
            return -1;
        }
    }

    else {
        // Extension already activated
        dxinfox[ channum ].state = ST_ADMINADDF;
        dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_extexists.pcm", O_RDONLY);
        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
            file_error(channum, "sounds/activation/activationivr_admin_extexists.pcm");
            return -1;
        }
        return 0;
    }
    return 0;
}

int admincount_cb(void * chanptr, int argc, char **argv, char **coldata)
{
    int channum = *((int *) chanptr);
    int count = atoi(argv[0]);
    if (count == 0) {
        // SQL database returned zero entries for that query.
        // Someone's pulling our leg here; the username clearly doesn't exist.
        return admin_cb_invalid(channum);
    }
    else if (count == 1) {
        char query[45];
        char * err_msg;
        snprintf( query, 44, "SELECT * FROM USERS WHERE EXTENSION = %s;", dxinfox[ channum ].digbuf.dg_value);
        if (sqlite3_exec( activationdb, query, admin_cb, (void*) &channum, &err_msg) != SQLITE_OK) {
            disp_msgf("SQL SELECT ERROR: %s", err_msg);
            dxinfox[channum].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            return -1;
        }
        else return 0;
    }
    else {
        // There's more than one of these. What happened exactly?
        char query[53];
        char * err_msg;
        // Uhhhm, let's just... yeah.
        snprintf( query, 52, "SELECT * FROM USERS WHERE EXTENSION = %s LIMIT 1;", filetmp[channum]);
        if (sqlite3_exec( activationdb, query, admin_cb, (void*) &channum, &err_msg) != SQLITE_OK) {
            disp_msgf("SQL SELECT ERROR: %s", err_msg);
            dxinfox[channum].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            return -1;
        }
        else return 0;
    }
}

