#ifndef config_h
#define config_h

#include "stdint.h"

/*
 *config.h
 * this file contains information that is usually taken from ModelDescription.xml
 */

// define class name and unique id
#define MODEL_IDENTIFIER Clocks
#define MODEL_GUID "{8c4e810f-3df3-4a00-8276-176fa3c9f000}"

// define model size
#define NUMBER_OF_STATES 0
#define NUMBER_OF_EVENT_INDICATORS 0

#define N_INPUT_CLOCKS  3
typedef enum {
	InClock_1 = 0,
	InClock_2 = 1,
	InClock_3 = 2
} InputClockEnum;

/* clock priorities as defined in ModelDescription.xml */
typedef enum {
	InClock_1_Prio = 3,   // smaller values => higher Prio 
	InClock_2_Prio = 33,
	InClock_3_Prio = 63
} InputClockPrioEnum;

// definitions for the independent clocks (InClock_3 depends on OutClock_1)
#define N_OUTPUT_CLOCKS 2
typedef enum {
	OutClock_1 = 0,
	OutClock_2 = 1
} OutputClockEnum;

/*
 * #defines for the implemented functions of the FMI3.0 framework
 * for these functions, the (mostly empty) default implementation will not be used
*/
#define SET_INT32
#define GET_INT32
#define GET_CLOCK
#define ACTIVATE_MODEL_PARTITION

// these times are taken from <DefaultExperiment> in ModelDescription.xml
#define START_TIME 0
#define STOP_TIME 10
#define FIXED_SOLVER_STEP 1


typedef enum {
	vr_InClock_1 = 1001,
	vr_InClock_2 = 1002,
	vr_InClock_3 = 1003,
	vr_OutClock_1 = 1004,
	vr_OutClock_2 = 1005,
	vr_InClock_1_Ticks = 2001,
	vr_InClock_2_Ticks = 2002,
	vr_InClock_3_Ticks = 2003,
	vr_total_InClock_Ticks = 2004,
	vr_result_2 = 2005,
	vr_input_2 = 2006,
	vr_output_3 = 2007
} ValueReference;

typedef struct {
    int data_InClock_1;
    int data_InClock_2;
    int data_InClock_3;
	int data_OutClock_1;
	int data_OutClock_2;
	int data_InClock_1_Ticks;
	int data_InClock_2_Ticks;
	int data_InClock_3_Ticks;
    int data_total_InClock_Ticks;
	int data_result_2; 
	int data_input_2;
	int data_output_3;
} ModelData;

typedef struct threadArgs_t {
	void * comp;
	ValueReference clockRef; 
	unsigned int *retval;
	double activationTime;
} ThreadArgs;

#endif /* config_h */
