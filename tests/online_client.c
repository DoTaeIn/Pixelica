#define _POSIX_C_SOURCE 200809L
#include "pixelica/online.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void pause_ms(unsigned ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void pause_ms(unsigned ms) {
    struct timespec t = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    (void)nanosleep(&t, NULL);
}
#endif

#define REQUIRE(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static int checks;
static int members(PIXELICA_Online *c, unsigned count) {
    unsigned i;
    for(i=0;i<500u;++i) {
        PIXELICA_OnlineSnapshot s;
        if(pixelica_online_snapshot(c,&s)==0 && s.connected && s.player_count==count) return 1;
        pause_ms(10);
    }
    return 0;
}
static int board_done(PIXELICA_Online *c, uint32_t previous, PIXELICA_OnlineLeaderboard *out) {
    unsigned i;
    for(i=0;i<500u;++i) {
        (void)pixelica_online_leaderboard(c,out);
        if(out->generation>previous) return out->status==0;
        pause_ms(10);
    }
    return 0;
}
int main(int argc,char **argv) {
    PIXELICA_OnlineConfig cfg=pixelica_online_config_default();
    PIXELICA_Online *clients[8]={0}, *extra;
    PIXELICA_OnlineStatus error;
    PIXELICA_OnlineSnapshot snap;
    PIXELICA_OnlineLeaderboard board;
    uint8_t payload[2048];
    char key[65],code[7];
    unsigned i,j;
    int mode;
    REQUIRE(argc>=2);
    cfg.server_url=argv[1]; cfg.game_id="audit"; cfg.player_name="host";
    cfg.timeout_ms=500;
    if(argc==4 && strcmp(argv[2],"--expect-status")==0) {
        extra=pixelica_online_host(&cfg,(PIXELICA_OnlineMode)1,8,&error);
        if(extra) pixelica_online_destroy(extra);
        REQUIRE(extra==NULL); REQUIRE((int)error==atoi(argv[3]));
        return 0;
    }
    if(argc==3 && strcmp(argv[2],"--reject-tls")==0) {
        extra=pixelica_online_host(&cfg,(PIXELICA_OnlineMode)1,8,&error);
        REQUIRE(extra==NULL); REQUIRE(error==100);
        puts("untrusted TLS rejected"); return 0;
    }
    for(mode=1;mode<=2;++mode) {
        clients[0]=pixelica_online_host(&cfg,(PIXELICA_OnlineMode)mode,8,&error);
        REQUIRE(clients[0]!=NULL); REQUIRE(error==0);
        memcpy(code,pixelica_online_room_code(clients[0]),7);
        REQUIRE(strlen(code)==6);
        for(i=1;i<8;++i) {
            cfg.player_name="guest";
            clients[i]=pixelica_online_join(&cfg,code,&error);
            REQUIRE(clients[i]!=NULL); REQUIRE(error==0);
        }
        REQUIRE(members(clients[0],8));
        extra=pixelica_online_join(&cfg,code,&error);
        REQUIRE(extra==NULL); REQUIRE(error==3);
        cfg.game_id="othergame";
        extra=pixelica_online_join(&cfg,code,&error);
        REQUIRE(extra==NULL); REQUIRE(error==2);
        cfg.game_id="audit";
        for(i=0;i<sizeof payload;++i) payload[i]=(uint8_t)(i&255u);
        REQUIRE(pixelica_online_state(clients[1],payload,1)==5);
        REQUIRE(pixelica_online_input(clients[0],payload,65)==103);
        REQUIRE(pixelica_online_state(clients[0],payload,2049)==103);
        REQUIRE(pixelica_online_state(clients[0],payload,sizeof payload)==0);
        for(i=0;i<8;++i) {
            payload[0]=(uint8_t)i;
            REQUIRE(pixelica_online_input(clients[i],payload,64)==0);
        }
        for(i=0;i<8;++i) {
            int found=0;
            for(j=0;j<500;++j) {
                unsigned k,valid=0;
                (void)pixelica_online_snapshot(clients[i],&snap);
                for(k=0;k<snap.player_count;++k)
                    if(snap.players[k].input_size==64 && snap.players[k].input[0]==snap.players[k].id) ++valid;
                if(valid==8 && snap.state_size==2048 && snap.state[0]==0 && snap.state[2047]==255) {found=1;break;}
                pause_ms(10);
            }
            REQUIRE(found);
        }
        REQUIRE(pixelica_online_input(clients[7],NULL,0)==0);
        REQUIRE(pixelica_online_state(clients[0],NULL,0)==0);
        for(j=0;j<500;++j) {
            (void)pixelica_online_snapshot(clients[7],&snap);
            if(snap.state_size==0 && snap.state_sequence>1) break;
            pause_ms(10);
        }
        REQUIRE(j<500);
        if(mode==1) {
            memcpy(key,pixelica_online_player_key(clients[7]),65);
            REQUIRE(strlen(key)==64);
            (void)pixelica_online_leaderboard(clients[7],&board);
            REQUIRE(pixelica_online_submit_score(clients[7],"speed",100,"record",6)==0);
            REQUIRE(board_done(clients[7],board.generation,&board));
            REQUIRE(board.count==1 && board.entries[0].score==100);
            REQUIRE(board.entries[0].record_size==6 && memcmp(board.entries[0].record,"record",6)==0);
            pixelica_online_destroy(clients[7]); clients[7]=NULL;
            REQUIRE(members(clients[0],7));
            cfg.player_key=key;
            clients[7]=pixelica_online_join(&cfg,code,&error);
            REQUIRE(clients[7]!=NULL); REQUIRE(strcmp(pixelica_online_player_key(clients[7]),key)==0);
            cfg.player_key=NULL;
            REQUIRE(pixelica_online_submit_score(clients[7],"speed",50,"worse",5)==0);
            REQUIRE(board_done(clients[7],0,&board));
            REQUIRE(board.count==1 && board.entries[0].score==100);
            REQUIRE(board.entries[0].record_size==6);
            pause_ms(600);
            REQUIRE(pixelica_online_submit_score(clients[7],"speed",200,"better",6)==0);
            REQUIRE(board_done(clients[7],board.generation,&board));
            REQUIRE(board.count==1 && board.entries[0].score==200);
            REQUIRE(memcmp(board.entries[0].record,"better",6)==0);
            REQUIRE(pixelica_online_request_board(clients[0],"speed")==0);
            REQUIRE(board_done(clients[0],0,&board));
            REQUIRE(board.entries[0].score==200);
        }
        pixelica_online_destroy(clients[0]);clients[0]=NULL;
        for(j=0;j<500;++j) {
            PIXELICA_OnlineStatus st=pixelica_online_snapshot(clients[1],&snap);
            if(st!=0 && !snap.connected) break;
            pause_ms(10);
        }
        REQUIRE(j<500);
        for(i=1;i<8;++i) {pixelica_online_destroy(clients[i]);clients[i]=NULL;}
        cfg.player_name="host";
    }
    printf("online client integration: %d checks, coop + versus, 8 clients each\n",checks);
    return 0;
}
