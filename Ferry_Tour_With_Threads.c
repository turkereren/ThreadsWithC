

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

/* ------------------------ constants ------------------------ */
#define NUM_CARS     12
#define NUM_MINI     10
#define NUM_TRUCK     8
#define CAR_W         1
#define MINI_W        2
#define TRUCK_W       3
#define FERRY_CAP    20

#define SIDES         2
#define TOLLS_PS      2
#define TOLLS_TOTAL  (SIDES * TOLLS_PS)

/* fake timings (µs) just to slow log output */
#define PAY_US   600000
#define BOARD_US 500000
#define SAIL_US 1500000
#define REST_MIN 400000
#define REST_MAX 1200000

/* --------------------- helpers ----------------------------- */
static inline int rnd(int upper){ return rand() % upper; }

static void nap(useconds_t us){
    struct timespec ts = { us/1000000, (us%1000000)*1000 };
    nanosleep(&ts, NULL);
}

/* ------------------ vehicle struct ------------------------- */
typedef struct vehicle{
    int id, w;
    const char *lab;
    int home, cur, trips;
    pthread_mutex_t m;
    pthread_cond_t  cv_on, cv_off;
    struct vehicle *next;
} veh_t;

/* simple FIFO queue */
typedef struct{ veh_t *h,*t; pthread_mutex_t m; } queue_t;
static void q_init(queue_t *q){ q->h=q->t=NULL; pthread_mutex_init(&q->m,NULL); }
static void q_push(queue_t *q, veh_t *v){
    v->next=NULL;
    pthread_mutex_lock(&q->m);
    if(q->t) q->t->next=v; else q->h=v;
    q->t=v;
    pthread_mutex_unlock(&q->m);
}
static veh_t *q_pop_fit(queue_t *q,int room){
    pthread_mutex_lock(&q->m);
    veh_t *prev=NULL,*cur=q->h;
    while(cur){
        if(cur->w<=room){
            if(prev) prev->next=cur->next; else q->h=cur->next;
            if(q->t==cur) q->t=prev;
            pthread_mutex_unlock(&q->m);
            return cur;
        }
        prev=cur; cur=cur->next;
    }
    pthread_mutex_unlock(&q->m);
    return NULL;
}

/* ----------------- globals ------------------ */
static pthread_mutex_t toll_m[TOLLS_TOTAL];
static queue_t square[SIDES];
static pthread_cond_t cv_side[SIDES];

static pthread_mutex_t deck_m = PTHREAD_MUTEX_INITIALIZER;
static int ferry_side = 0, ferry_load = 0;
static veh_t *deck[FERRY_CAP]; int deck_cnt = 0;

static int remaining = NUM_CARS+NUM_MINI+NUM_TRUCK;
static pthread_mutex_t rem_m = PTHREAD_MUTEX_INITIALIZER;

/* ----------------- ferry thread ------------- */
static void *ferry(void *arg){
    (void)arg;
    while(1){
        /* wait until vehicles on current side or done */
        pthread_mutex_lock(&square[ferry_side].m);
        while(!square[ferry_side].h){
            pthread_mutex_lock(&rem_m);
            if(!remaining){ pthread_mutex_unlock(&rem_m); pthread_mutex_unlock(&square[ferry_side].m); return NULL;}
            pthread_mutex_unlock(&rem_m);
            pthread_cond_wait(&cv_side[ferry_side], &square[ferry_side].m);
        }
        pthread_mutex_unlock(&square[ferry_side].m);

        /* loading */
        pthread_mutex_lock(&deck_m);
        ferry_load=0; deck_cnt=0;
        printf("\n>> Boarding at side %d\n", ferry_side);
        while(ferry_load < FERRY_CAP){
            int room = FERRY_CAP - ferry_load;
            veh_t *v = q_pop_fit(&square[ferry_side], room);
            if(!v) break;
            deck[deck_cnt++] = v;
            ferry_load += v->w;
            pthread_mutex_lock(&v->m);
            pthread_cond_signal(&v->cv_on);
            pthread_mutex_unlock(&v->m);
            nap(BOARD_US);
        }
        printf(">> Depart (%d CU, %d veh)\n", ferry_load, deck_cnt);
        pthread_mutex_unlock(&deck_m);

        nap(SAIL_US);
        ferry_side ^= 1;
        printf(">> Arrive side %d\n", ferry_side);

        /* unloading */
        pthread_mutex_lock(&deck_m);
        for(int i=0;i<deck_cnt;i++){
            pthread_mutex_lock(&deck[i]->m);
            pthread_cond_signal(&deck[i]->cv_off);
            pthread_mutex_unlock(&deck[i]->m);
            nap(BOARD_US);
        }
        deck_cnt = 0;
        pthread_mutex_unlock(&deck_m);
    }
}

/* ---------------- vehicle thread ------------ */
static void *veh(void *arg){
    veh_t *me = arg;
    while(1){
        /* pay */
        int booth = me->cur * TOLLS_PS + rnd(TOLLS_PS);
        pthread_mutex_lock(&toll_m[booth]);
        printf("%s-%d pays at booth %d (side %d)\n", me->lab, me->id, booth, me->cur);
        nap(PAY_US);
        pthread_mutex_unlock(&toll_m[booth]);

        /* queue */
        q_push(&square[me->cur], me);
        pthread_cond_signal(&cv_side[me->cur]);

        /* wait board / unload */
        pthread_mutex_lock(&me->m);
        pthread_cond_wait(&me->cv_on,  &me->m);
        pthread_cond_wait(&me->cv_off, &me->m);
        pthread_mutex_unlock(&me->m);

        me->cur ^= 1;
        printf("%s-%d off ferry (side %d)\n", me->lab, me->id, me->cur);

        if(me->trips){
            if(me->cur == me->home){
                pthread_mutex_lock(&rem_m);
                --remaining;
                pthread_mutex_unlock(&rem_m);
                return NULL;
            }
        } else me->trips = 1;

        nap(REST_MIN + rnd(REST_MAX - REST_MIN));
    }
}

/* ---------------- helper to build fleet ------ */
static veh_t *make(int n,int w,const char*lab,int*idc){
    veh_t *a = calloc(n,sizeof *a);
    for(int i=0;i<n;i++){
        a[i].id=*idc; (*idc)++;
        a[i].w=w; a[i].lab=lab;
        a[i].home = a[i].cur = rnd(2);
        pthread_mutex_init(&a[i].m, NULL);
        pthread_cond_init (&a[i].cv_on,  NULL);
        pthread_cond_init (&a[i].cv_off, NULL);
    }
    return a;
}

/* --------------------- main ------------------ */
int main(void){
    srand((unsigned)time(NULL));

    for(int i=0;i<TOLLS_TOTAL;i++) pthread_mutex_init(&toll_m[i],NULL);
    for(int s=0;s<SIDES;s++){ q_init(&square[s]); pthread_cond_init(&cv_side[s],NULL); }

    int idc=0;
    veh_t *car   = make(NUM_CARS,  CAR_W,  "Car" , &idc);
    veh_t *mini  = make(NUM_MINI,  MINI_W, "Mini", &idc);
    veh_t *truck = make(NUM_TRUCK, TRUCK_W,"Truck", &idc);

    pthread_t ferry_tid;
    pthread_create(&ferry_tid, NULL, ferry, NULL);

    int tot = NUM_CARS+NUM_MINI+NUM_TRUCK;
    pthread_t tid[tot]; int k=0;
    for(int i=0;i<NUM_CARS;  i++) pthread_create(&tid[k++],NULL,veh,&car[i]);
    for(int i=0;i<NUM_MINI;  i++) pthread_create(&tid[k++],NULL,veh,&mini[i]);
    for(int i=0;i<NUM_TRUCK; i++) pthread_create(&tid[k++],NULL,veh,&truck[i]);

    for(int i=0;i<tot;i++) pthread_join(tid[i],NULL);
    pthread_cond_broadcast(&cv_side[0]);
    pthread_cond_broadcast(&cv_side[1]);
    pthread_join(ferry_tid,NULL);

    puts("\n*** All trips finished. ***");
    return 0;
}
