#include "pixelica/online.h"
#include "../modules/online/online_internal.h"
#include <stdio.h>
#include <string.h>

static int checks,failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while (0)
static void invalid(PIXELICA_OnlineConfig *cfg) {
    PIXELICA_OnlineStatus error=PIXELICA_ONLINE_OK;
    PIXELICA_Online *c=pixelica_online_host(cfg,PIXELICA_ONLINE_COOP,2,&error);
    CHECK(c==NULL);CHECK(error==PIXELICA_ONLINE_INVALID_ARG);
    pixelica_online_destroy(c);
}
static void parser_checks(void) {
    uint8_t bytes[4096];
    uint32_t rng=0x7217a20du;
    unsigned trial;
    PIXELICA_OnlineSnapshot out,before;
    PIXELICA_OnlineLeaderboard board,board_before;
    /* Valid one-player snapshot, independent of the client encoder. */
    const uint8_t good[]={0,0,0,1, 0,1,1,8, 0,0,0,0, 0,0, 0,4,'h','o','s','t',0,0,0,0,0};
    memset(&out,0xa5,sizeof out);before=out;
    for(trial=0;trial<sizeof good;++trial) {
        CHECK(pixelica_online_internal_parse_snapshot(good,trial,0,&out)==PIXELICA_ONLINE_PROTOCOL);
        CHECK(memcmp(&out,&before,sizeof out)==0);
    }
    CHECK(pixelica_online_internal_parse_snapshot(good,sizeof good,0,&out)==PIXELICA_ONLINE_OK);
    CHECK(out.player_count==1 && strcmp(out.players[0].name,"host")==0);
    memset(&board,0xa5,sizeof board);board_before=board;
    for(trial=0;trial<16000u;++trial) {
        size_t j,n;
        PIXELICA_OnlineStatus st;
        rng=rng*1664525u+1013904223u;n=(size_t)(rng%4097u);
        for(j=0;j<n;++j) {rng=rng*1664525u+1013904223u;bytes[j]=(uint8_t)(rng>>24);}
        before=out;
        st=pixelica_online_internal_parse_snapshot(bytes,n,0,&out);
        CHECK(st==PIXELICA_ONLINE_PROTOCOL || st==PIXELICA_ONLINE_OK);
        if(st!=PIXELICA_ONLINE_OK) CHECK(memcmp(&out,&before,sizeof out)==0);
        board_before=board;
        st=pixelica_online_internal_parse_board(bytes,n,"audit",&board);
        CHECK(st==PIXELICA_ONLINE_PROTOCOL || st==PIXELICA_ONLINE_OK);
        if(st!=PIXELICA_ONLINE_OK) CHECK(memcmp(&board,&board_before,sizeof board)==0);
    }
}
int main(void) {
    PIXELICA_OnlineConfig cfg=pixelica_online_config_default();
    PIXELICA_OnlineSnapshot snap;
    PIXELICA_OnlineLeaderboard board;
    PIXELICA_OnlineStatus error;
    size_t i;
    const char *urls[]={"", "ftp://127.0.0.1", "http://example.com", "http://localhost.evil",
        "http://127.0.0.1.evil", "http://127.1", "http://2130706433", "http://[::1].evil",
        "https://user:password@localhost", "https://localhost?key=secret", "https://localhost/#frag",
        "https://localhost\r\nX-Header: bad", "https://localhost:0", "https://localhost:65536"};
    CHECK(cfg.timeout_ms>=100 && cfg.timeout_ms<=10000);
    CHECK(cfg.sync_hz==30);CHECK(!cfg.allow_insecure_lan);
    cfg.game_id="audit";cfg.player_name="player";
    CHECK(strcmp(pixelica_online_room_code(NULL),"")==0);
    CHECK(strcmp(pixelica_online_player_key(NULL),"")==0);
    pixelica_online_destroy(NULL);
    CHECK(pixelica_online_input(NULL,NULL,0)==PIXELICA_ONLINE_INVALID_ARG);
    CHECK(pixelica_online_state(NULL,NULL,0)==PIXELICA_ONLINE_INVALID_ARG);
    CHECK(pixelica_online_snapshot(NULL,&snap)==PIXELICA_ONLINE_INVALID_ARG);
    CHECK(pixelica_online_leaderboard(NULL,&board)==PIXELICA_ONLINE_INVALID_ARG);
    CHECK(pixelica_online_submit_score(NULL,"score",0,NULL,0)==PIXELICA_ONLINE_INVALID_ARG);
    CHECK(pixelica_online_request_board(NULL,"score")==PIXELICA_ONLINE_INVALID_ARG);
    for(i=0;i<sizeof urls/sizeof urls[0];++i) {cfg.server_url=urls[i];invalid(&cfg);}
    cfg=pixelica_online_config_default(); cfg.server_url="http://127.0.0.1:9";
    cfg.game_id="bad game";invalid(&cfg);cfg.game_id="good";
    cfg.player_name="\xc0\xaf";invalid(&cfg);
    cfg.player_name="\xed\xa0\x80";invalid(&cfg);
    cfg.player_name="\xf4\x90\x80\x80";invalid(&cfg);
    cfg.player_name="\xc2\x85";invalid(&cfg);
    cfg.player_name="name\n";invalid(&cfg);cfg.player_name="valid";
    cfg.player_key="bad";invalid(&cfg);cfg.player_key=NULL;
    cfg.timeout_ms=99;invalid(&cfg);cfg.timeout_ms=10001;invalid(&cfg);cfg.timeout_ms=100;
    cfg.sync_hz=0;invalid(&cfg);cfg.sync_hz=61;invalid(&cfg);cfg.sync_hz=30;
    CHECK(pixelica_online_host(&cfg,(PIXELICA_OnlineMode)3,2,&error)==NULL);
    CHECK(error==PIXELICA_ONLINE_INVALID_ARG);
    CHECK(pixelica_online_host(&cfg,PIXELICA_ONLINE_COOP,1,&error)==NULL);
    CHECK(error==PIXELICA_ONLINE_INVALID_ARG);
    CHECK(pixelica_online_join(&cfg,"ABC",&error)==NULL);CHECK(error==PIXELICA_ONLINE_INVALID_ARG);
    CHECK(pixelica_online_join(&cfg,"IIIIII",&error)==NULL);CHECK(error==PIXELICA_ONLINE_INVALID_ARG);
    CHECK(pixelica_online_status_string((PIXELICA_OnlineStatus)999)!=NULL);
    parser_checks();
    printf("online validation: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
