#include <linux/module.h>
#include <net/tcp.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#define numOfState 3

#define state0_max 200 // throughput
#define state1_max 4  // delay, 0-5,6-20,21-
#define state2_max 200 // min_rtt

#define Q_CONG_SCALE 1024

#define numOfAction 3

#define epsilon 1

#define	sizeOfMatrix 	state0_max * state1_max * state2_max * numOfAction

static const u32 probertt_interval_msec = 10000;
static const u32 max_probertt_duration_msecs = 200;

static const u32 alpha = 1;
static const u32 beta = 1;
static const u32 gamma = 1;

static const char procname[] = "satcc";

static const int learning_rate = 512;
static const int discount_factor = 12;

#define MY_SAVE_FILE "/qtable-save-file"
#define MY_READ_FILE "/qtable-read-file"

enum action
{
	CWND_UP,
	CWND_DOWN,
	CWND_NOTHING,
};

enum q_cong_mode
{
	NOTHING,
	TRAINING,
	ESTIMATE_MIN_RTT,
	STARTUP,
};

typedef struct
{
	u8 enabled;
	int mat[sizeOfMatrix];
	u8 row[numOfState];
	u8 col;
} Matrix;

static Matrix matrix;

struct Q_cong
{
	u64 alpha;
	bool forced_update;

	u32 mode : 3,
		exited : 1,
		up_times : 4,
		down_times : 4,
		up_n : 3,
		epsilon_step : 6,
		epsilon_count : 4,
		unused : 11;
	u32 last_sequence;
	u32 estimated_throughput;
	u16 last_throughput_mean[5];
	u32 last_update_stamp;
	u32 last_packet_loss;
	u32 retransmit_during_interval;

    u8 throughput_count;
	u32 smooth_throughput;

	u32 last_probertt_stamp;
	u32 start_up_stamp;
	u32 min_rtt_us;
	u32 prop_rtt_us;
	u16 prior_cwnd;

	u16 current_state[numOfState];
	u16 prev_state[numOfState];
	u8 action;
};

static int matrix_init = 0;
static void createMatrix(Matrix *m, u16 *row, u16 col)
{
	u32 i;

	if (!m)
		return;

	m->col = col;

	for (i = 0; i < numOfState; i++)
		*(m->row + i) = *(row + i);

	// use matrix repeatedly
	if (matrix_init == 0)
	{
		for (i = 0; i < sizeOfMatrix; i++)
			*(m->mat + i) = 0;
		matrix_init = 1;
	}

	m->enabled = 1;
}

static void eraseMatrix(Matrix *m)
{
	u8 i = 0;
	if (!m)
		return;

	for (i = 0; i < numOfState; i++)
		*(m->row + i) = 0;

	m->col = 0;
	m->enabled = 0;
}

static void setMatValue(Matrix *m, u16 row1, u16 row2, u16 row3, u16 col, int v){
	u32 index = 0; 
	if (!m)
		return;

	index = m->col * (row1 * m->row[1] * m->row[2] + row2 * m->row[2] + row3) + col;
	*(m -> mat + index) = v;
}

static int getMatValue(Matrix *m, u16 row1, u16 row2, u16 row3, u16 col){
	u32 index = 0; 
	if (!m)
		return -1; 

	index = m->col * (row1 * m->row[1] * m->row[2] + row2 * m->row[2] + row3) + col;
	
	return *(m -> mat + index);
}

static void save_Matrix(Matrix *m)
{
	struct file *fp;
	loff_t pos;
	ssize_t res = 0;
	printk(KERN_INFO "write enter/n");
	fp = filp_open(MY_SAVE_FILE, O_RDWR | O_CREAT, 0777);
	if (IS_ERR(fp))
	{
		printk(KERN_INFO "create file error/n");
		return;
	}

	pos = 0;
	res = kernel_write(fp, m, sizeof(Matrix), &pos);
	if (res < 0)
	{
		printk(KERN_INFO "kernel_write error: %ld\n", res);
		return;
	}

	filp_close(fp, NULL);
}

static void read_Matrix(Matrix *m)
{
	struct file *fp;
	loff_t pos;
	ssize_t res = 0;
	printk(KERN_INFO "reader enter/n");
	fp = filp_open(MY_READ_FILE, O_RDWR | O_CREAT, 0777);
	if (IS_ERR(fp))
	{
		printk(KERN_INFO "create file error/n");
		return;
	}

	pos = 0;
	res = kernel_read(fp, m, sizeof(Matrix), &pos);
	if (res < 0)
	{
		printk("kernel_read error: %ld\n", res);
		return;
	}
	printk("read: %p/n", &m);

	filp_close(fp, NULL);
}

static u32 q_cong_ssthresh(struct sock *sk)
{
	return TCP_INFINITE_SSTHRESH; /* TCP Q-congestion does not use ssthresh */
}

static u32 epsilon_expore(struct sock *sk, u32 max_index)
{	
	struct Q_cong *qc = inet_csk_ca(sk);
	u32 rand;
	u32 rand2;
	u32 random_value;
	get_random_bytes(&rand, sizeof(rand));
	random_value = (rand % (10 * (1 + qc->epsilon_step)));
	if (random_value >= epsilon)
		return max_index;
	get_random_bytes(&rand2, sizeof(rand2));
	return rand2 % numOfAction;
}

static void epsilon_update(struct sock *sk, const struct rate_sample *rs){
	struct Q_cong *qc = inet_csk_ca(sk);
	u8 i;
	u32 sum_throughput;
	u32 change_th;
	u32 change_rtt;

	sum_throughput = 0;
	for (i = 0; i < 5; i++)
		sum_throughput += qc->last_throughput_mean[i];
	change_th = ((int)(sum_throughput/5 - (qc->smooth_throughput>>5)) >= 0) ? (int)(sum_throughput/5 - (qc->smooth_throughput>>5)) : (int)((qc->smooth_throughput>>5) - sum_throughput/5);
	
	change_rtt = ((rs->rtt_us - qc->min_rtt_us)>>10) >= 0 ? (rs->rtt_us - qc->min_rtt_us)>>10 : (qc->min_rtt_us - rs->rtt_us)>>10;
	
	if(qc->epsilon_count<15){
		qc->epsilon_count++;
	}
	if(qc->epsilon_count==15){
		if(qc->epsilon_step<64){
			qc->epsilon_step++;
		}
		qc->epsilon_count = 0;
	}

	if((change_th > (qc->smooth_throughput>>8)) || (change_rtt > (qc->min_rtt_us>>13))){
		qc->epsilon_step = 0;
	}
}

int softsigntt(int value, int throughput)
{
	if(value == 0) value=1;
    return value * 100 / (value + throughput);
}

static u32 getAction(struct sock *sk, const struct rate_sample *rs)
{
	struct Q_cong *qc = inet_csk_ca(sk);

	u32 Q[numOfAction];
	u8 i;
	u8 is_equal = 1;
	u32 max_index = 0;
	u32 max_tmp = 0;
	u32 rand;

	for (i = 0; i < numOfAction; i++)
	{
		Q[i] = getMatValue(&matrix, qc->current_state[0], qc->current_state[1], qc->current_state[2], i);
	}

	max_tmp = Q[0];
	for (i = 0; i < numOfAction; i++)
	{
		if (max_tmp == Q[i])
		{
			max_index = i;
			continue;
		}
		is_equal = 0;
		if (max_tmp < Q[i])
		{
			max_tmp = Q[i];
			max_index = i;
		}
	}

	if (is_equal)
	{
		get_random_bytes(&rand, sizeof(rand));
		max_index = (rand % numOfAction);
	}

	return epsilon_expore(sk, max_index);
}

static int getRewardFromEnvironment(struct sock *sk, const struct rate_sample *rs)
{
	struct Q_cong *qc = inet_csk_ca(sk);
	u32 retransmit_division_factor;
	int result;
	u8 i;
	u32 sum_throughput;
	u32 goodness;
	int delay;
	int fire;

	retransmit_division_factor = qc->retransmit_during_interval + 1;
	if (retransmit_division_factor == 0 || rs->rtt_us == 0)
		return 0;
	if(qc->estimated_throughput>0) fire=retransmit_division_factor/(qc->estimated_throughput);
	else fire = 0;

	sum_throughput = 0;
	for(i=0;i<5;i++){
		sum_throughput += qc->last_throughput_mean[i];
	}

	goodness = softsigntt(qc->estimated_throughput>>5 , (sum_throughput/5)); // 0-99

	delay = (rs->rtt_us - qc->min_rtt_us)>>10;
	
	result = alpha * goodness - beta * delay - gamma * 100 * fire;

	// printk(KERN_INFO "reward : %d, goodness: %d, fire: %d, delay: %d,min_rtt: %d, throughput>>5: %d", result, goodness, fire, delay, qc->min_rtt_us>>10 , qc->estimated_throughput >> 5);

	return result;
}

static int up_actions_list[8] = {30,150,750,3750,18750,93750,468750,2343750};
static int down_actions_list[8] = {1,3,5,9,15,21,33,51};
static void executeAction(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct Q_cong *qc = inet_csk_ca(sk);
	u32 a;

	switch (qc->action)
	{
	case CWND_UP:

		if(tp->snd_cwnd==0) tp->snd_cwnd=1;
		a = up_actions_list[qc->up_n] / tp->snd_cwnd;
		if(a==0) a=1;
		tp->snd_cwnd = tp->snd_cwnd + a;

		if(qc->up_times<15){
			qc->up_times ++;
		}
		qc->down_times = 0;
							
        if( qc->up_times>2){
            if(qc->up_n<7){
                qc->up_n++;
            }
        }

		break;

	case CWND_DOWN:
		if(qc->down_times<8){
			if(tp->snd_cwnd > down_actions_list[qc->down_times]){
				tp->snd_cwnd = tp->snd_cwnd - down_actions_list[qc->down_times];
			}
		}else{
			if(tp->snd_cwnd>1){
				tp->snd_cwnd = tp->snd_cwnd - (tp->snd_cwnd>>1);
			}
		}

		if(qc->down_times<15){
			qc->down_times ++;
		}
		qc->up_times = 0;

		if(qc->up_n>0){
                qc->up_n--;
            }

		break;

	default:
		if(qc->up_n>0){
                qc->up_n--;
            }
		break;
	}

}

static u32 q_cong_undo_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd, tp->prior_cwnd);
}

static void calc_retransmit_during_interval(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct Q_cong *qc = inet_csk_ca(sk);
	u32 training_interval_msec;
	training_interval_msec = 2 * (qc->min_rtt_us>>10); //2RTT

	qc->retransmit_during_interval = (tp->total_retrans - qc->last_packet_loss) * training_interval_msec / jiffies_to_msecs(tcp_jiffies32 - qc->last_update_stamp);
	qc->last_packet_loss = tp->total_retrans;
}

static void calc_throughput(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct Q_cong *qc = inet_csk_ca(sk);
	u8 i;
	u32 segout_for_interval;

	segout_for_interval = (tp->segs_out - qc->last_sequence) * tp->mss_cache;

	qc->estimated_throughput = segout_for_interval * 8 / jiffies_to_msecs(tcp_jiffies32 - qc->last_update_stamp);

	for (i = 0; i < 4; i++)
		qc->last_throughput_mean[i] = qc->last_throughput_mean[i+1];
	if((qc->estimated_throughput>>5)<65535) 	//u16
		qc->last_throughput_mean[4] = qc->estimated_throughput>>5;
	else
		qc->last_throughput_mean[4] = 65534;

	// smooth_throughput
	if(qc->smooth_throughput==0) qc->smooth_throughput=qc->estimated_throughput; //first time
	qc->smooth_throughput = (qc->smooth_throughput - (qc->smooth_throughput>>3)) + (qc->estimated_throughput>>3);

	qc->last_sequence = tp->segs_out;

}

static void update_state(struct sock *sk, const struct rate_sample *rs)
{	
	struct tcp_sock *tp = tcp_sk(sk);
	struct Q_cong *qc = inet_csk_ca(sk);
	u8 i;
	int delay;

	for (i = 0; i < numOfState; i++)
		qc->prev_state[i] = qc->current_state[i];

	qc->current_state[0] = qc->estimated_throughput>>9; // 0-100Mbps

	qc->current_state[2] = qc->min_rtt_us>>12; // (rtt(0-800)>>10 ms)/4 = (0-200)

	// delay 0-80 >>2 20
	delay = (rs->rtt_us - qc->min_rtt_us) >> 10;
	if(delay<=0){
		qc->current_state[1] = 0;
	}else if(delay<=5 && delay>0){
		qc->current_state[1] = 1;
	}else if(delay>=6 && delay<=20){
		qc->current_state[1] = 2;
	}else if(delay>20){
		qc->current_state[1] = 3;
	}

	if (qc->current_state[0] < 0)
		qc->current_state[0] = 0;
	else if (qc->current_state[0] > 199)
		qc->current_state[0] = 199;

	if (qc->current_state[1] < 0)
		qc->current_state[1] = 0;
	else if (qc->current_state[1] > 3)
		qc->current_state[1] = 3;

	if (qc->current_state[2] < 0)
		qc->current_state[2] = 0;
	else if (qc->current_state[2] > 199)
		qc->current_state[2] = 199;

}

static void update_Qtable(struct sock *sk, const struct rate_sample *rs)
{
	struct Q_cong *qc = inet_csk_ca(sk);

	int thisQ[numOfAction];
	int newQ[numOfAction];
	u8 i;
	int updated_Qvalue;
	int max_tmp;
	for (i = 0; i < numOfAction; i++)
	{
		thisQ[i] = getMatValue(&matrix, qc->prev_state[0], qc->prev_state[1], qc->prev_state[2], i);
		newQ[i] = getMatValue(&matrix, qc->current_state[0], qc->current_state[1], qc->current_state[2], i);
		// printk(KERN_INFO "i: %u, this Q %d, newQ: %d", i ,thisQ[i] ,newQ[i]);
	}

	max_tmp = newQ[0];
	for (i = 0; i < numOfAction; i++)
	{
		if (max_tmp < newQ[i])
			max_tmp = newQ[i];
	}
	updated_Qvalue = ((Q_CONG_SCALE - learning_rate) * thisQ[qc->action] +
					  (learning_rate * (getRewardFromEnvironment(sk, rs) + ((discount_factor * max_tmp) >> 4)))) >>
					 10;

	setMatValue(&matrix, qc->prev_state[0], qc->prev_state[1], qc->prev_state[2], qc->action, updated_Qvalue);
}

static void training(struct sock *sk, const struct rate_sample *rs)
{
	struct Q_cong *qc = inet_csk_ca(sk);
	u32 training_interval_msec;
	u32 training_timer_expired;
	training_interval_msec = 2 * (qc->min_rtt_us>>10); //2RTT

	training_timer_expired = after(tcp_jiffies32, qc->last_update_stamp + msecs_to_jiffies(training_interval_msec));

	if (training_timer_expired && qc->mode == NOTHING)
	{
		if (qc->action == 0xffffffff)
			goto execute;

		calc_throughput(sk);
		update_state(sk, rs);
		calc_retransmit_during_interval(sk);

		update_Qtable(sk, rs);
	execute:
		// printk(KERN_INFO "execute Action: %u", qc -> action);
		qc->action = getAction(sk, rs);
		executeAction(sk, rs);
		qc->last_update_stamp = tcp_jiffies32;
		
		epsilon_update(sk, rs);
	}
}

static void update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct Q_cong *qc = inet_csk_ca(sk);
	u32 estimate_rtt_expired;

	u32 update_filter_expired = after(tcp_jiffies32,
									  qc->last_probertt_stamp + msecs_to_jiffies(probertt_interval_msec));

	if (rs->rtt_us > 0)
	{
		if (rs->rtt_us < qc->min_rtt_us)
		{
			qc->min_rtt_us = rs->rtt_us;
			if(qc->mode != ESTIMATE_MIN_RTT){
				qc->last_probertt_stamp = tcp_jiffies32;
			}
			if (qc->min_rtt_us < qc->prop_rtt_us)
				qc->prop_rtt_us = qc->min_rtt_us;
		}
	}

	if (update_filter_expired && qc->mode == NOTHING)
	{
		qc->mode = ESTIMATE_MIN_RTT;
		qc->last_probertt_stamp = tcp_jiffies32;
		qc->prior_cwnd = tp->snd_cwnd;
		// tp->snd_cwnd = min(tp->snd_cwnd, estimate_min_rtt_cwnd);
		tp->snd_cwnd = 1 + (tp->snd_cwnd>>2);
		qc->min_rtt_us = rs->rtt_us;
	}

	if (qc->mode == ESTIMATE_MIN_RTT)
	{
		estimate_rtt_expired = after(tcp_jiffies32,
									 qc->last_probertt_stamp + msecs_to_jiffies(max_probertt_duration_msecs));
		if (estimate_rtt_expired)
		{
			qc->mode = NOTHING;
			tp->snd_cwnd = qc->prior_cwnd;
		}
	}
}

static void reset_cwnd(struct sock *sk, const struct rate_sample *rs){
	struct Q_cong *qc = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	u32 stop_start_up = after(tcp_jiffies32,
									  qc->start_up_stamp + msecs_to_jiffies(2000));

	if (qc -> mode == STARTUP){
		if(inet_csk(sk) -> icsk_ca_state >= TCP_CA_Recovery){
			qc -> mode = NOTHING; 
		}
		else{
			tp -> snd_cwnd += rs -> acked_sacked;
		}
	}

	if(stop_start_up && qc -> mode == STARTUP){
		qc -> mode = NOTHING; 
	}
}

static void q_cong_main(struct sock *sk, const struct rate_sample *rs)
{
	reset_cwnd(sk, rs);
	training(sk, rs);
	update_min_rtt(sk, rs);
}

static void init_Q_cong(struct sock *sk)
{
	struct Q_cong *qc;
	struct tcp_sock *tp = tcp_sk(sk);
	u16 Q_row[numOfState] = {state0_max, state1_max, state2_max};
	u16 Q_col = numOfAction;

	qc = inet_csk_ca(sk);

	qc->mode = STARTUP;
	qc->up_times = 0;
	qc->down_times = 0;
	qc->up_n = 0;
	qc->epsilon_step = 0;
	qc->epsilon_count = 0;
	qc->last_sequence = 0;
	qc->estimated_throughput = 0;

	qc->last_throughput_mean[0]=0;
	qc->last_throughput_mean[1]=0;
	qc->last_throughput_mean[2]=0;
	qc->last_throughput_mean[3]=0;
	qc->last_throughput_mean[4]=0;

	qc->last_update_stamp = tcp_jiffies32;
	qc->last_packet_loss = 0;
	qc->start_up_stamp = tcp_jiffies32;

    qc -> throughput_count = 0;
	qc -> smooth_throughput = 0;

	qc->last_probertt_stamp = tcp_jiffies32;
	qc->min_rtt_us = tcp_min_rtt(tp);
	qc->prop_rtt_us = tcp_min_rtt(tp);
	qc->prior_cwnd = 0;
	qc->retransmit_during_interval = 0;

	qc->action = -1;
	qc->exited = 0;
	qc->prev_state[0] = 0;
	qc->prev_state[1] = 0;
	qc->prev_state[2] = 0;
	qc->current_state[0] = 0;
	qc->current_state[1] = 0;
	qc->current_state[2] = 0;

	// createMatrix(&matrix, Q_row, Q_col);
	read_Matrix(&matrix);
}

static void release_Q_cong(struct sock *sk)
{
	eraseMatrix(&matrix);
}

struct tcp_congestion_ops q_cong = {
	.flags = TCP_CONG_NON_RESTRICTED,
	.init = init_Q_cong,
	.release = release_Q_cong,
	.name = "satcc",
	.owner = THIS_MODULE,
	.ssthresh = q_cong_ssthresh,
	.cong_control = q_cong_main,
	.undo_cwnd = q_cong_undo_cwnd,
};

static int __init Q_cong_init(void)
{
	BUILD_BUG_ON(sizeof(struct Q_cong) > ICSK_CA_PRIV_SIZE); 
	return tcp_register_congestion_control(&q_cong);
}

static void __exit Q_cong_exit(void)
{
	// save_Matrix(&matrix);
	tcp_unregister_congestion_control(&q_cong);
}

module_init(Q_cong_init);
module_exit(Q_cong_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("HuiQi,JinyaoLiu");
MODULE_DESCRIPTION("SATCC: A Congestion Control Algorithm for Dynamic Satellite Networks");
