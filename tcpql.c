#include <linux/module.h>
#include <net/tcp.h>
#include <linux/math64.h>

#define numOfState	3

#define	state0_max	10	// throughput relative value
#define	state1_max	19	// throughput diff value
#define	state2_max	19	// rtt diff value

#define	TCP_QL_SCALE	1024

#define	numOfAction	4

#define epsilon 8   // Explore parameters 0~9 <= epsilon

#define	sizeOfMatrix 	state0_max * state1_max * state2_max * numOfAction

static const u32 probertt_interval_msec = 10000;
static const u32 training_interval_msec = 100;
static const u32 max_probertt_duration_msecs = 200;
static const u32 estimate_min_rtt_cwnd = 4;

static const u32 alpha = 200;
static const u32 beta = 1;
static const u32 delta = 1; 

static const char procname[] = "tcpql";

static const u32 learning_rate = 512;
static const u32 discount_factor = 12; 

enum action{
	CWND_UP_30,
	CWND_UP_1,
	CWND_DOWN,
    CWND_NOTHING,
};

enum tcp_ql_mode{
	NOTHING,
	TRAINING,
	ESTIMATE_MIN_RTT,
	STARTUP,
};

typedef struct{
	u8  enabled;
	int mat[sizeOfMatrix];	//本身就是int，为什么不存负值得效用函数呢？
	u8 row[numOfState];
	u8 col;
}Matrix; 

static Matrix matrix;

struct Tcp_ql{
	u64	alpha;
	bool	forced_update;
	
	u32	mode:3,
		exited:1,
		unused:28;
	u32 	last_sequence; 
	u32	estimated_throughput;
	u32 smooth_throughput;
	u32	last_update_stamp;
	u32	last_packet_loss;
	u32 	retransmit_during_interval; 

	u32	last_probertt_stamp;
	u32 pre_rtt; 
	u32 	min_rtt_us; 
	u32	prop_rtt_us;
	u32	prior_cwnd;

	u32	current_state[numOfState];
	u32	prev_state[numOfState];
	u32 	action; 
};


static int matrix_init = 0;
static void createMatrix(Matrix *m, u8 *row, u8 col){
	u32 i;
	
	if (!m)
		return;

	m->col = col; 

	for(i=0; i<numOfState; i++)
		*(m->row+i) = *(row+i);

	// use matrix repeatedly
	if(matrix_init == 0){
		for(i=0; i<sizeOfMatrix; i++)
			*(m->mat + i) = 0;
		matrix_init = 1;
	}

	m -> enabled = 1; 
}

static void eraseMatrix(Matrix *m){
	u8 i=0; 
	if (!m)
		return;

	for(i=0; i<numOfState; i++)
		*(m->row + i) = 0; 

	m -> col = 0; 
	m -> enabled = 0; 
}

static void setMatValue(Matrix *m, u8 row1, u8 row2, u8 row3, u8 col, int v){
	u32 index = 0; 
	if (!m)
		return;
	// row1 * m->row[1] * m->row[2] + row2 * m->row[2] + row3
	index = m->col * (row1 * m->row[1] * m->row[2] + row2 * m->row[2]  + row3 ) + col;
	*(m -> mat + index) = v;
}

static int getMatValue(Matrix *m, u8 row1, u8 row2, u8 row3,  u8 col){
	u32 index = 0; 
	if (!m)
		return -1; 

	index = m->col * (row1 * m->row[1] * m->row[2] + row2 * m->row[2]  + row3) + col;
	
	return *(m -> mat + index);
}

static u32 tcp_ql_ssthresh(struct sock *sk){
	return TCP_INFINITE_SSTHRESH; /* TCP Q-congestion does not use ssthresh */
}

static void reset_cwnd(struct sock *sk, const struct rate_sample *rs){
	struct Tcp_ql *tql = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	if (tql -> mode == STARTUP){
		if(inet_csk(sk) -> icsk_ca_state >= TCP_CA_Recovery){
			tql -> mode = NOTHING; 
		}
		else{
			tp -> snd_cwnd += rs -> acked_sacked;
		}
	}
}

static u32 epsilon_expore(u32 max_index){
	u32 rand;
	u32 rand2;
	u32 random_value;
	get_random_bytes(&rand, sizeof(rand));
	random_value = (rand%10); // 0~9
	if(random_value <= epsilon)
		return max_index;
	get_random_bytes(&rand2, sizeof(rand2));
	return (rand2%numOfAction);
}

int softsignt(int value){	// softsign for throughput while caculate reward
	u32 remainder;
	return (div_s64_rem(value*10, ((value<0?~(--value):value) + 2000), &remainder));		// -9~9 | 2000 is the best value for throughput diff
}

int softsignr(int value){	// softsign for rtt while caculate reward
	u32 remainder;
	return (div_s64_rem(value*10, ((value<0?~(--value):value) + 800), &remainder));		// -9~9 | 800 is the best value for delay diff
}

int softsign(int value){    // softsign for others relative value in state
	u32 remainder;
	return (div_s64_rem(value*10, ((value<0?~(--value):value)+1000), &remainder)) + 9;		// 0-19 状态值
}

int softsigntt(int value, int smooth_throughput){	// softsign for throughput relative value in state
	u32 remainder;
	return div_s64_rem((value==0?1:value)*10, ((value==0?1:value) + smooth_throughput), &remainder);	// 0-9 状态值	smooth_throughout as parm
}


static u32 getAction(struct sock *sk, const struct rate_sample *rs){
	struct Tcp_ql *tql = inet_csk_ca(sk);

	u32 Q[numOfAction];
	u8 i;
	u8 is_equal = 1;
	u32 max_index = 0; 
	u32 max_tmp = 0 ;
	u32 rand;	

	for(i=0; i<numOfAction; i++){
		Q[i] = getMatValue(&matrix, tql -> current_state[0], tql->current_state[1], tql->current_state[2],i);
	}

	max_tmp = Q[0];
	for(i=0; i<numOfAction; i++){
		if(max_tmp == Q[i]){
			max_index = i;
			continue;
		}
		is_equal = 0;
		if(max_tmp < Q[i]){
			max_tmp = Q[i];
			max_index = i;
		}
	}
	
	if(is_equal){
		get_random_bytes(&rand, sizeof(rand));
		max_index = (rand%numOfAction);
	}

	if(max_index == CWND_DOWN){
		return max_index;
	}

	return epsilon_expore(max_index);
}

static int getRewardFromEnvironment(struct sock *sk, const struct rate_sample *rs){
	//struct tcp_sock *tp = tcp_sk(sk);
	struct Tcp_ql *tql = inet_csk_ca(sk);
	u32 retransmit_division_factor; 
    int diff_throughput;
    int diff_delay;
    int smooth_divide_current_throughput;
    int result;

	retransmit_division_factor = tql -> retransmit_during_interval + 1;
	if(retransmit_division_factor == 0 || rs->rtt_us == 0)
		return 0;
	
	diff_throughput = softsignt((int)(tql -> estimated_throughput - tql -> smooth_throughput));
	diff_delay = softsignr((int)((rs -> rtt_us - tql -> min_rtt_us)-(tql -> pre_rtt - tql -> min_rtt_us)));	// measurement inaccuracy
	smooth_divide_current_throughput =  (int)(tql -> smooth_throughput / (tql -> estimated_throughput==0?1:tql -> estimated_throughput)) > 20 ? 20 : (int)(tql -> smooth_throughput / (tql -> estimated_throughput==0?1:tql -> estimated_throughput));

    /* 
	 * Utility Function
	 *
	 * Utility = 3 * diff_throughput - diff_delay  - smooth_divide_current_throughput
	 *
	 */

	result = 3 * diff_throughput - diff_delay - smooth_divide_current_throughput;
	
	printk(KERN_INFO "reward : %d", result);
	
	return result;
}

static void executeAction(struct sock *sk, const struct rate_sample *rs){
	struct tcp_sock *tp = tcp_sk(sk);
	struct Tcp_ql *tql = inet_csk_ca(sk);

	switch(tql -> action){
		case CWND_UP_30:
			tp -> snd_cwnd  = tp->snd_cwnd + 30/ (tp->snd_cwnd);
			break;

		case CWND_UP_1:
			tp -> snd_cwnd  = tp->snd_cwnd + 1;
			break;

		case CWND_DOWN:
			tp -> snd_cwnd = tp->snd_cwnd - (tp->snd_cwnd >> 1);
			break; 

		default : 
			break;
	}
}

static u32 tcp_ql_undo_cwnd(struct sock* sk){
	struct tcp_sock *tp = tcp_sk(sk);
	
	// printk(KERN_INFO "undo congestion control");
	return max(tp->snd_cwnd, tp->prior_cwnd);
}

static void calc_retransmit_during_interval(struct sock* sk){
	struct tcp_sock *tp = tcp_sk(sk);
	struct Tcp_ql *tql = inet_csk_ca(sk);

	tql -> retransmit_during_interval = (tp -> total_retrans - tql -> last_packet_loss) * training_interval_msec / jiffies_to_msecs(tcp_jiffies32 - tql -> last_update_stamp);
	tql -> last_packet_loss = tp -> total_retrans; 
}

static void calc_throughput(struct sock *sk){
	struct tcp_sock *tp = tcp_sk(sk);
	struct Tcp_ql *tql = inet_csk_ca(sk);

	u32 segout_for_interval;
	
	segout_for_interval = (tp -> segs_out - tql ->last_sequence) * tp ->mss_cache; 

	tql -> estimated_throughput = segout_for_interval * 8 / jiffies_to_msecs(tcp_jiffies32 - tql -> last_update_stamp); 
	tql -> smooth_throughput = ((7 * tql -> smooth_throughput)>>3) + ((tql -> estimated_throughput)>>3);		// 1/8
	tql -> last_sequence = tp -> segs_out;
}

static int update_state(struct sock *sk, const struct rate_sample *rs){
	struct Tcp_ql *tql = inet_csk_ca(sk);
    int current_rtt;
	u8 i; 

	for (i=0; i<numOfState; i++)
		tql -> prev_state[i] = tql -> current_state[i];
	
	tql -> current_state[0] = softsigntt((int)tql -> estimated_throughput, (int)tql -> smooth_throughput);
	tql -> current_state[1] = softsign((int)(tql -> estimated_throughput - tql -> smooth_throughput));
	current_rtt = rs->rtt_us;
	tql -> current_state[2] = softsign((int)(current_rtt - tql-> pre_rtt));		// pre_rtt是比smoothrtt好的，但是这里的问题是一秒一取造成了pre很不准确
	return current_rtt;
}

static void update_Qtable(struct sock *sk, const struct rate_sample *rs){
	struct Tcp_ql *tql = inet_csk_ca(sk);

	u32 thisQ[numOfAction]; 
	u32 newQ[numOfAction];
	u8 i;
	int updated_Qvalue;
	int max_tmp; 
	
	for(i=0; i<numOfAction; i++){
		thisQ[i] = getMatValue(&matrix, tql->prev_state[0], tql->prev_state[1], tql->prev_state[2], i);
		newQ[i] = getMatValue(&matrix, tql->current_state[0], tql->current_state[1], tql->current_state[2], i);
	}

	max_tmp = newQ[0];
	for(i=0; i<numOfAction; i++){
		if(max_tmp < newQ[i])
			max_tmp = newQ[i]; 
	}

	updated_Qvalue = ((TCP_QL_SCALE-learning_rate)*thisQ[tql ->action] +
			(learning_rate * (getRewardFromEnvironment(sk,rs) + (discount_factor * max_tmp)>>4 - thisQ[tql -> action] )))>>10;

	if(updated_Qvalue == 0){
		tql -> exited = 1; 
		return;
	}
	
	setMatValue(&matrix, tql->prev_state[0], tql->prev_state[1], tql->prev_state[2], tql->action, updated_Qvalue);
}

static void training(struct sock *sk, const struct rate_sample *rs){
	struct tcp_sock *tp = tcp_sk(sk);
	struct Tcp_ql *tql = inet_csk_ca(sk);
	
	u32 training_timer_expired = after(tcp_jiffies32, tql -> last_update_stamp + msecs_to_jiffies(training_interval_msec)); 

	if(training_timer_expired && tql -> mode == NOTHING){

		if (tql -> action == 0xffffffff)
			goto execute;

		calc_throughput(sk);
		calc_retransmit_during_interval(sk);

		if (tql -> exited == 1){
			tp -> snd_cwnd = TCP_INIT_CWND; 
			tql -> exited = 0; 
			return; 
		}

		update_Qtable(sk,rs);
execute:
		printk(KERN_INFO "execute Action: %u", tql -> action);
		tql -> action = getAction(sk,rs);
		executeAction(sk, rs);
		tql -> last_update_stamp = tcp_jiffies32; 
	}
}

static void update_min_rtt(struct sock *sk, const struct rate_sample* rs){
	struct tcp_sock *tp = tcp_sk(sk);
	struct Tcp_ql *tql = inet_csk_ca(sk);
	u32 estimate_rtt_expired; 

	u32 update_filter_expired = after(tcp_jiffies32, 
			tql -> last_probertt_stamp + msecs_to_jiffies(probertt_interval_msec));

	if (rs -> rtt_us > 0){	
		if (rs -> rtt_us < tql -> min_rtt_us){
			tql -> min_rtt_us = rs -> rtt_us;
			tql -> last_probertt_stamp = tcp_jiffies32; 
			if (tql -> min_rtt_us < tql-> prop_rtt_us)
				tql -> prop_rtt_us = tql -> min_rtt_us; 
		}
	}

	if(update_filter_expired && tql -> mode == NOTHING){ 
		tql -> mode = ESTIMATE_MIN_RTT; 
		tql -> last_probertt_stamp = tcp_jiffies32; 
		tql -> prior_cwnd = tp -> snd_cwnd;
		tp -> snd_cwnd = min(tp -> snd_cwnd, estimate_min_rtt_cwnd);
		tql -> min_rtt_us = rs -> rtt_us;
	}

	if(tql -> mode == ESTIMATE_MIN_RTT){
		estimate_rtt_expired = after(tcp_jiffies32, 
				tql -> last_probertt_stamp + msecs_to_jiffies(max_probertt_duration_msecs)); 
		if(estimate_rtt_expired){
			tql -> mode = NOTHING; 
			tp -> snd_cwnd = tql -> prior_cwnd;
		}
	}
}

static void tcp_ql_main(struct sock *sk, const struct rate_sample *rs){
	// struct tcp_sock *tp = tcp_sk(sk);
	struct Tcp_ql *tql = inet_csk_ca(sk);
    int current_rtt;

	reset_cwnd(sk, rs);
	current_rtt = update_state(sk,rs);
	training(sk, rs);
	tql -> pre_rtt = current_rtt;
	update_min_rtt(sk,rs);
}



static void init_Tcp_ql(struct sock *sk){
	struct Tcp_ql *tql;
	struct tcp_sock *tp = tcp_sk(sk);
	u8 Qtable_row[numOfState] = {state0_max, state1_max, state2_max};
	u8 Qtable_col = numOfAction; 

	tql = inet_csk_ca(sk);

	tql -> mode = STARTUP;
	tql -> last_sequence = 0;
	tql -> estimated_throughput = 0;
	tql -> smooth_throughput = 0;
	tql -> last_update_stamp = tcp_jiffies32;
	tql -> last_packet_loss = 0;

	tql -> last_probertt_stamp = tcp_jiffies32;
	tql -> min_rtt_us = tcp_min_rtt(tp);
	tql -> prop_rtt_us = tcp_min_rtt(tp);
	tql -> pre_rtt = tcp_min_rtt(tp);
	tql -> prior_cwnd = 0;
	tql -> retransmit_during_interval = 0;

	tql -> action = -1; 
	tql -> exited = 0; 
	tql -> prev_state[0] = 0;
	tql -> prev_state[1] = 0; 
	tql -> prev_state[2] = 0;
	tql -> current_state[0] = 0;
	tql -> current_state[1] = 0;
	tql -> current_state[2] = 0;

	createMatrix(&matrix, Qtable_row, Qtable_col);
}

static void release_Tcp_ql(struct sock* sk){
	eraseMatrix(&matrix);
}

struct tcp_congestion_ops tcp_ql = {
	.flags		= TCP_CONG_NON_RESTRICTED, 
	.init		= init_Tcp_ql,
	.release	= release_Tcp_ql,
	.name 		= "tcpql",
	.owner		= THIS_MODULE,
	.ssthresh	= tcp_ql_ssthresh,
	.cong_control	= tcp_ql_main,
	.undo_cwnd 	= tcp_ql_undo_cwnd,
};

static int __init Tcp_ql_init(void){
	BUILD_BUG_ON(sizeof(struct Tcp_ql) > ICSK_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&tcp_ql);
}

static void __exit Tcp_ql_exit(void){
	tcp_unregister_congestion_control(&tcp_ql);
}

module_init(Tcp_ql_init);
module_exit(Tcp_ql_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("HuiQi,JinyaoLiu");
MODULE_DESCRIPTION("tcpql : Learning based Congestion Control Algorithm");
