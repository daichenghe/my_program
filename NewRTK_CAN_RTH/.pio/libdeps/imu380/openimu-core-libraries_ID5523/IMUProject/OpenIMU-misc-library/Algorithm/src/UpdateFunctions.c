/*
 * File:   UpdateFunctions.c
 * Author: joemotyka
 *
 * Created on May 8, 2016, 12:35 AM
 */

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "GlobalConstants.h"   // TRUE, FALSE, NUMBER_OF_EKF_STATES, ...
#include "Indices.h"
#include "StateIndices.h"

#include "platformAPI.h"
#include "TimingVars.h"

#include "VectorMath.h"
#include "MatrixMath.h"
#include "QuaternionMath.h"
#include "TransformationMath.h"

#include "algorithm.h"
#include "algorithmAPI.h"
#include "AlgorithmLimits.h"
#include "EKF_Algorithm.h"
#include "UpdateFunctions.h"
#include "SensorNoiseParameters.h"


extern AccelStatsStruct gAccelStats;

#if FAST_MATH
#include "arm_math.h"
#endif // FAST_MATH

#ifdef INS_OFFLINE
#include "SimulationParameters.h"
#else
#ifdef DISPLAY_DIAGNOSTIC_MSG
#include "debug.h"
#endif
#endif // INS_OFFLINE


// H is sparse and has elements in the following locations...
uint8_t RLE_H[ROWS_IN_H][2] = { {STATE_Q0, STATE_Q3},
                                {STATE_Q0, STATE_Q3},
                                {STATE_Q0, STATE_Q3} };

// KxH is sparse with elements only in cols 6 through 9
uint8_t RLE_KxH[ROWS_IN_K][2] = { {STATE_Q0, STATE_Q3}, {STATE_Q0, STATE_Q3}, {STATE_Q0, STATE_Q3},
                                  {STATE_Q0, STATE_Q3}, {STATE_Q0, STATE_Q3}, {STATE_Q0, STATE_Q3},
                                  {STATE_Q0, STATE_Q3}, {STATE_Q0, STATE_Q3}, {STATE_Q0, STATE_Q3}, {STATE_Q0, STATE_Q3},
                                  {STATE_Q0, STATE_Q3}, {STATE_Q0, STATE_Q3}, {STATE_Q0, STATE_Q3},
                                  {STATE_Q0, STATE_Q3}, {STATE_Q0, STATE_Q3}, {STATE_Q0, STATE_Q3} };

// Local functions
static void _TurnSwitch(void);

static real _UnwrapAttitudeError( real attitudeError );
static real _LimitValue( real value, real limit );
static BOOL _CheckForUpdateTrigger(uint8_t updateRate);

/******************************************************************************
 * @brief Initializa heading when GPS velocity is above a certain threshhold.
 * If the GPS velocity is above a certain threshold, a global variable useGpsHeading
 * will be set to 1. And in this function, the heading will be initialized to be
 * gEKFInputData.trueCourse, and velocity will also be initiazlied as the
 * corresponding NED speed. After this, the quaternion (q0 and q3) and velocity
 * terms in the state covariance matrix P will be reset. Non-diagonal terms will be
 * set as 0s, and diagonal terms will be set according to estimated variance. The
 * cov(quaternion, velocity) should also be updated. But the positive-definiteness
 * is not guaranteed this way.
 * TRACE:
 * @retval TRUE if heading initialized, FALSE if not.
******************************************************************************/
static int InitializeGnssHeading();

// Update rates
#define  TEN_HERTZ_UPDATE          10
#define  TWENTY_HERTZ_UPDATE       20
#define  TWENTY_FIVE_HERTZ_UPDATE  25
#define  FIFTY_HERTZ_UPDATE        50
#define  ONE_HUNDRED_HERTZ_UPDATE  100

static uint32_t updateCntr[2] = { 0, 0 };
static BOOL useGpsHeading = 0;  /* When GPS velocity is above a certain threshold,
                                 * this is set to 1, and GPS heading measurement
                                 * is used, otherwise, this is set to 0 and magnetic
                                 * heading is used.    */
static int runInsUpdate = 0;    /* To enable the update to be broken up into
                                 * two sequential calculations in two sucessive
                                 * 100 Hz periods. */

// Uncomment to run only AHRS-type updates
//#define ATT_UPDATE_ONLY

static void Update_GPS(void);
static void Update_PseudoVel(void);

// EKF_UpdateStage.m
void EKF_UpdateStage(void)
{
    /* Perform a VG/AHRS update, regardless of GPS availability or health,
     * when the state is HG AHRS or LG AHRS.  Once GPS becomes healthy
     * (and the right conditions are met) perform an INS or reduced-order GPS update.
     */
    if( gAlgorithm.state <= LOW_GAIN_AHRS )
    {
        // Only allow the algorithm to be called on 100 Hz marks
        if(timer.oneHundredHertzFlag == 1) 
        {
            // Update the AHRS solution at a 10 Hz update rate
            // Subframe counter counts to 10 before it is reset
            if( _CheckForUpdateTrigger(TEN_HERTZ_UPDATE) )
            {
                /* The AHRS/VG solution is handled inside FieldVectorsToEulerAngles
                 * (called from the prediction function EKF_PredictionStage)
                 */
                ComputeSystemInnovation_Att();
                Update_Att();
            }
        }
    } 
    else
    {
        /* GPS-type Updates (with magnetometers: true-heading = mag-heading + mag-decl)
         * Perform the EKF update at 10 Hz (split nine mag-only updates for for every GPS/mag update)
         * 
         * Check for 'new GPS data'. If new, and GPS is valid, perform a
         * GPS-Based update and reset timer values to resync the attitude updates.
         */
        if( gEKFInputData.gpsUpdate )
        {   
            if (gEKFInputData.gpsValid)
            {
                Update_GPS();
            }
            else
            {
                Update_PseudoVel();
            }
            // At 1 Hz mark, update when GPS data is valid, else do an AHRS-update
            runInsUpdate = 1;
        }
        else if( runInsUpdate )
        {
            Update_Att();
            runInsUpdate = 0;  // set up for next pass
            useGpsHeading = 0;
        }

        // Update LLA at 100/200 Hz
        if ( gEKFInputData.gpsValid && ( gAlgorithm.insFirstTime == FALSE ) ) 
        {
            //r_E = Base_To_ECEF( &gKalmanFilter.Position_N[0], &gAlgorithm.rGPS0_E[0], &R_NinE[0][0] );    //
            double r_E[NUM_AXIS];
            float pointOfInterestN[3];
            pointOfInterestN[0] = gKalmanFilter.R_BinN[0][0] * gAlgorithm.pointOfInterestB[0] +
                gKalmanFilter.R_BinN[0][1] * gAlgorithm.pointOfInterestB[1] +
                gKalmanFilter.R_BinN[0][2] * gAlgorithm.pointOfInterestB[2];
            pointOfInterestN[1] = gKalmanFilter.R_BinN[1][0] * gAlgorithm.pointOfInterestB[0] +
                gKalmanFilter.R_BinN[1][1] * gAlgorithm.pointOfInterestB[1] +
                gKalmanFilter.R_BinN[1][2] * gAlgorithm.pointOfInterestB[2];
            pointOfInterestN[2] = gKalmanFilter.R_BinN[2][0] * gAlgorithm.pointOfInterestB[0] +
                gKalmanFilter.R_BinN[2][1] * gAlgorithm.pointOfInterestB[1] +
                gKalmanFilter.R_BinN[2][2] * gAlgorithm.pointOfInterestB[2];
            pointOfInterestN[0] += gKalmanFilter.Position_N[0];
            pointOfInterestN[1] += gKalmanFilter.Position_N[1];
            pointOfInterestN[2] += gKalmanFilter.Position_N[2];
            PosNED_To_PosECEF(pointOfInterestN, &gAlgorithm.rGPS0_E[0], &gAlgorithm.R_NinE[0][0], &r_E[0] );
            //                 100 Hz           generated once          1 Hz                      100 Hz

            //gKalmanFilter.llaDeg[LAT_IDX] = ECEF2LLA( r_E );   // output variable (ned used for anything else); this is in [ deg, deg, m ]
            ECEF_To_LLA(&gKalmanFilter.llaDeg[LAT], &r_E[X_AXIS]);
            //          100 Hz                    100 Hz
        }
    }
}

// ----Compute the innovation vector, nu----
void ComputeSystemInnovation_Pos(void)
{
    // Position error
    gKalmanFilter.nu[STATE_RX] = gAlgorithm.rGPS_N[X_AXIS] - gKalmanFilter.Position_N[X_AXIS];
    gKalmanFilter.nu[STATE_RY] = gAlgorithm.rGPS_N[Y_AXIS] - gKalmanFilter.Position_N[Y_AXIS];
    gKalmanFilter.nu[STATE_RZ] = gAlgorithm.rGPS_N[Z_AXIS] - gKalmanFilter.Position_N[Z_AXIS];

    gKalmanFilter.nu[STATE_RX] = _LimitValue(gKalmanFilter.nu[STATE_RX], gAlgorithm.Limit.Innov.positionError);
    gKalmanFilter.nu[STATE_RY] = _LimitValue(gKalmanFilter.nu[STATE_RY], gAlgorithm.Limit.Innov.positionError);
    gKalmanFilter.nu[STATE_RZ] = _LimitValue(gKalmanFilter.nu[STATE_RZ], gAlgorithm.Limit.Innov.positionError);
}


// ----Compute the innovation vector, nu----
void ComputeSystemInnovation_Vel(void)
{
    // Velocity error
    gKalmanFilter.nu[STATE_VX] = (real)gEKFInputData.vNed[X_AXIS] - gKalmanFilter.Velocity_N[X_AXIS];
    gKalmanFilter.nu[STATE_VY] = (real)gEKFInputData.vNed[Y_AXIS] - gKalmanFilter.Velocity_N[Y_AXIS];
    gKalmanFilter.nu[STATE_VZ] = (real)gEKFInputData.vNed[Z_AXIS] - gKalmanFilter.Velocity_N[Z_AXIS];

    gKalmanFilter.nu[STATE_VX] = _LimitValue(gKalmanFilter.nu[STATE_VX], gAlgorithm.Limit.Innov.velocityError);
    gKalmanFilter.nu[STATE_VY] = _LimitValue(gKalmanFilter.nu[STATE_VY], gAlgorithm.Limit.Innov.velocityError);
    gKalmanFilter.nu[STATE_VZ] = _LimitValue(gKalmanFilter.nu[STATE_VZ], gAlgorithm.Limit.Innov.velocityError);
}


/* Compute the innovation, nu, between measured and predicted attitude.
 *   Correct for wrap-around. Then limit the error.
 */
void ComputeSystemInnovation_Att(void)
{
    // ----- Roll -----
    gKalmanFilter.nu[STATE_ROLL]  = gKalmanFilter.measuredEulerAngles[ROLL] -
                                    gKalmanFilter.eulerAngles[ROLL];
    gKalmanFilter.nu[STATE_ROLL]  = _UnwrapAttitudeError(gKalmanFilter.nu[STATE_ROLL]);
    gKalmanFilter.nu[STATE_ROLL] = _LimitValue(gKalmanFilter.nu[STATE_ROLL], gAlgorithm.Limit.Innov.attitudeError);

    // ----- Pitch -----
    gKalmanFilter.nu[STATE_PITCH] = gKalmanFilter.measuredEulerAngles[PITCH] -
                                    gKalmanFilter.eulerAngles[PITCH];
    gKalmanFilter.nu[STATE_PITCH] = _UnwrapAttitudeError(gKalmanFilter.nu[STATE_PITCH]);
    gKalmanFilter.nu[STATE_PITCH] = _LimitValue(gKalmanFilter.nu[STATE_PITCH], gAlgorithm.Limit.Innov.attitudeError);

    // ----- Yaw -----
    // CHANGED TO SWITCH BETWEEN GPS AND MAG UPDATES
    if ( useGpsHeading )
    {
        gKalmanFilter.nu[STATE_YAW] = (real)gEKFInputData.trueCourse * (real)DEG_TO_RAD -
                                          gKalmanFilter.eulerAngles[YAW];
    }
    else if ( magUsedInAlgorithm() && gAlgorithm.state <= LOW_GAIN_AHRS )
    {
        gKalmanFilter.nu[STATE_YAW]   = gKalmanFilter.measuredEulerAngles[YAW] -
                                        gKalmanFilter.eulerAngles[YAW];
    }
    else 
    {
        gKalmanFilter.nu[STATE_YAW] = (real)0.0;
    }
    gKalmanFilter.nu[STATE_YAW] = _UnwrapAttitudeError(gKalmanFilter.nu[STATE_YAW]);
    gKalmanFilter.nu[STATE_YAW] = _LimitValue(gKalmanFilter.nu[STATE_YAW], gAlgorithm.Limit.Innov.attitudeError);

    /* When the filtered yaw-rate is above certain thresholds then reduce the
     * attitude-errors used to update roll and pitch.
     */
    _TurnSwitch();

    //
    gKalmanFilter.nu[STATE_ROLL]  = gKalmanFilter.turnSwitchMultiplier * gKalmanFilter.nu[STATE_ROLL];
    gKalmanFilter.nu[STATE_PITCH] = gKalmanFilter.turnSwitchMultiplier * gKalmanFilter.nu[STATE_PITCH];
    //gKalmanFilter.nu[STATE_YAW]   = gKalmanFilter.turnSwitchMultiplier * gKalmanFilter.nu[STATE_YAW];
}


/******************************************************************************
 * @name: _GenerateObservationJacobian_RollAndPitch roll and pitch elements of
 *        the measurement Jacobian (H)
 * @brief
 * TRACE:
 * @param N/A
 * @retval 1
******************************************************************************/
uint8_t _GenerateObservationJacobian_AHRS(void)
{
    real xPhi, yPhi;
    real uTheta;
    real xPsi, yPsi;
    real denom = 1.0;
    real multiplier = 1.0;

    // Set the values in DP to zero
    static BOOL initH = TRUE;
    if( initH ) 
    {
        initH = FALSE;
        memset(gKalmanFilter.H, 0, sizeof(gKalmanFilter.H));
    }

    /// Note: H is 3x7
    /// Roll
    yPhi = (real)2.0 * ( gKalmanFilter.quaternion[Q2] * gKalmanFilter.quaternion[Q3] +
                         gKalmanFilter.quaternion[Q0] * gKalmanFilter.quaternion[Q1] );
    xPhi =  gKalmanFilter.quaternion[Q0] * gKalmanFilter.quaternion[Q0] + 
           -gKalmanFilter.quaternion[Q1] * gKalmanFilter.quaternion[Q1] +
           -gKalmanFilter.quaternion[Q2] * gKalmanFilter.quaternion[Q2] +
            gKalmanFilter.quaternion[Q3] * gKalmanFilter.quaternion[Q3];

    denom = yPhi*yPhi + xPhi*xPhi;
    if (denom < 1e-3) 
    {
        /* Based on drive-test data, the minimum value seen was 0.98 but the minimum
         * values based on Matlab analysis is 1e-7.
         */
        denom = (real)1e-3;
    }
    multiplier = (real)2.0 / denom;

    /// Derivative of the roll-angle wrt quaternions
    gKalmanFilter.H[ROLL][STATE_Q0] = multiplier * ( xPhi*gKalmanFilter.quaternion[Q1] +
                                                    -yPhi*gKalmanFilter.quaternion[Q0]);
    gKalmanFilter.H[ROLL][STATE_Q1] = multiplier * ( xPhi*gKalmanFilter.quaternion[Q0] + 
                                                     yPhi*gKalmanFilter.quaternion[Q1]);
    gKalmanFilter.H[ROLL][STATE_Q2] = multiplier * ( xPhi*gKalmanFilter.quaternion[Q3] + 
                                                     yPhi*gKalmanFilter.quaternion[Q2]);
    gKalmanFilter.H[ROLL][STATE_Q3] = multiplier * ( xPhi*gKalmanFilter.quaternion[Q2] +
                                                    -yPhi*gKalmanFilter.quaternion[Q3]);

    /* Pitch (including modifications for |q| = 1 constraint)
     *   mu = 2*( q(1)*q(3) - q(2)*q(4) );
     *   % 2/sqrt(1-mu*mu) * [q(3); -q(4); q(1); -q(2)]
     *   2/sqrt(1-mu*mu) * [q(3) - mu*q(1);
     *                      -q(4) - mu*q(2);
     *                     q(1)- mu*q(3);
     *                     -q(2)- mu*q(4)]
     */
    uTheta = (real)2.0 * ( gKalmanFilter.quaternion[Q1] * gKalmanFilter.quaternion[Q3] -
                      gKalmanFilter.quaternion[Q0] * gKalmanFilter.quaternion[Q2] );
    // account for numerical accuracy to make sure abs(uTheta) <= 1
    if (uTheta > 1.0)
    {
        uTheta = 1.0;
    }
    if (uTheta < -1.0)
    {
        uTheta = -1.0;
    }
    denom = (real)sqrt(1.0f - uTheta*uTheta);
    if (denom < 1e-3) 
    {
        denom = (real)1e-3;
    }
    multiplier = (real)2.0 / denom;

    gKalmanFilter.H[PITCH][STATE_Q0] = multiplier * ( gKalmanFilter.quaternion[Q2] + 
                                                      uTheta * gKalmanFilter.quaternion[Q0] );
    gKalmanFilter.H[PITCH][STATE_Q1] = multiplier * (-gKalmanFilter.quaternion[Q3] + 
                                                      uTheta * gKalmanFilter.quaternion[Q1] );
    gKalmanFilter.H[PITCH][STATE_Q2] = multiplier * ( gKalmanFilter.quaternion[Q0] + 
                                                      uTheta * gKalmanFilter.quaternion[Q2] );
    gKalmanFilter.H[PITCH][STATE_Q3] = multiplier * (-gKalmanFilter.quaternion[Q1] + 
                                                      uTheta * gKalmanFilter.quaternion[Q3] );

    /// Yaw
    yPsi = (real)2.0 * ( gKalmanFilter.quaternion[Q1] * gKalmanFilter.quaternion[Q2] +
                         gKalmanFilter.quaternion[Q0] * gKalmanFilter.quaternion[Q3] );
    xPsi = (real)1.0 - (real)2.0 * ( gKalmanFilter.quaternion[Q2] * gKalmanFilter.quaternion[Q2] +
                                     gKalmanFilter.quaternion[Q3] * gKalmanFilter.quaternion[Q3] );
    denom = yPsi*yPsi + xPsi*xPsi;
    if (denom < 1e-3) 
    {
        denom = (real)1e-3;
    }
    multiplier = (real)2.0 / denom;

    /// Derivative of the yaw-angle wrt quaternions
    gKalmanFilter.H[YAW][STATE_Q0] = multiplier * ( xPsi*gKalmanFilter.quaternion[Q3] +
                                                   -yPsi*gKalmanFilter.quaternion[Q0]);
    gKalmanFilter.H[YAW][STATE_Q1] = multiplier * ( xPsi*gKalmanFilter.quaternion[Q2] +
                                                   -yPsi*gKalmanFilter.quaternion[Q1]);
    gKalmanFilter.H[YAW][STATE_Q2] = multiplier * ( xPsi*gKalmanFilter.quaternion[Q1] + 
                                                    yPsi*gKalmanFilter.quaternion[Q2]);
    gKalmanFilter.H[YAW][STATE_Q3] = multiplier * ( xPsi*gKalmanFilter.quaternion[Q0] + 
                                                    yPsi*gKalmanFilter.quaternion[Q3]);

    return 1;
}


// 
void _GenerateObservationCovariance_AHRS(void)
{
    //
    static real Rnom;

    // Only need to compute certain elements of R once
    static BOOL initR = TRUE;
    if (initR) 
    {
        initR = FALSE;

        /* Clear the values in R (in AHRS mode, there are 3 rows in the Jacobian)
         * Initialize the Process Covariance (Q) matrix
         */
        memset(gKalmanFilter.R, 0, sizeof(gKalmanFilter.R));

#ifdef INS_OFFLINE
        /* This value is set based on the version string specified in the 
         * simulation configuration file, ekfSim.cfg
         */
        uint8_t sysRange = gSimulation.sysRange;
#else
        /* This value is set based on the version string loaded into the unit
         *   via the system configuration load
         */
        uint8_t sysRange = platformGetSysRange(); // from system config
#endif

        /* Set the matrix, R, based on whether the system is operating in high
         * or low-gain (is the acceleration above or below the acceleration threshold)
         *
         * R-values are based on the variance of the roll and pitch angles
         * generated from the sensor noise passed through the measurement
         * model.  The values are multiplied by dt^2 as well as vary with the
         * angle.  The value can be made large enough to work with all angles
         * but it may slow the response.
         *
         * These values are found by passing the accelerometer VRW values
         * (determined from a very limited data set) through a Matlab
         * script which generates the roll and pitch noise based on the
         * sensor noise.  The value below is the 1-sigma value at 0
         * degrees.  The quadratic correction below is meant to increase
         * R as the angle increases (due to the geometry and
         * mathematical function used to compute the angle).
         *
         * Matlab script: R_Versus_Theta.m
         */
        switch (sysRange) 
        {
            case _200_DPS_RANGE:
                // -200 VRW value (average x/y/z): 7.2e-4 [(m/sec)/rt-sec]
                Rnom = (real)9.82e-07;  // (9.91e-4)^2
                break;
            case _400_DPS_RANGE:
                    // -400 VRW value (average x/y/z): 8.8e-4 [(m/sec)/rt-sec]
                Rnom = (real)1.54e-06;  // (1.24e-3)^2
                break;
        }
    }

    /* Dynamically tune measurement covariance matrix R to get proper Kalman filter
     * gain. The R consists of four parts. First, its min value is Rnom; Secodd,
     * the online estimation of accel var; Thrid, the online estimation of accel
     * error; Four, the different between accel magnitude and 1g. 
     */

    // Rnom, accel var and accel error
    real totalAccelVar[3];
    for (int i = 0; i < 3; i++)
    {
        // replace sensor noise var with vibration var
        if (gAccelStats.accelVar[i] > Rnom)
        {
            totalAccelVar[i] = gAccelStats.accelVar[i];
        }
        else
        {
            totalAccelVar[i] = Rnom;
        }
        // linear accel? (including noise and vibration)
        real errSqr;
        errSqr = gAccelStats.accelErr[i] * gAccelStats.accelErr[i];
        if (errSqr > totalAccelVar[i])
        {
            totalAccelVar[i] = errSqr;
        }
    }

    /* consider magnitude to further increase R
     * Notice: totalAccelVarSum just approximates accel norm var
     */
    real totalAccelVarSum = totalAccelVar[X_AXIS] + totalAccelVar[Y_AXIS] + totalAccelVar[Z_AXIS];
    real diff = gAccelStats.accelNorm - 1.0f;
    diff *= diff;
    real additionalR = 0.0;
    /* if diff is larger than estimated accel err and the estimated accel err does
     * not reach limit, diff will be used as additional measurement noise var.
     */
    if (diff > 4.0*totalAccelVarSum && gAccelStats.accelErrLimit == false)
    {
        // the magnitude of diff is too big, there is linear acceleration
        additionalR = diff;
    }
    else
    {
        // the magnitude of diff is within noise level, no additional var
        additionalR = 0.0;
    }

    /* convert accel measurement var to pitch and roll var
     *  d(pitch) = 1/sqrt(1-ax^2) * d(ax) = 1/sqrt(ay^2+az^2) * d(ax)
     *  d(roll) = (az^2/(ay^2+az^2)) * d(ay) + (-ay/(ay^2+az^2)) * d(az)
     *  Notice: var(kx) = k*k*var(x)
     */
    // Get ax^2, ay^2 and az^2 of normalized accel
    real axSqr = gAccelStats.lpfAccel[0] * gAccelStats.lpfAccel[0];
    real aySqr = gAccelStats.lpfAccel[1] * gAccelStats.lpfAccel[1];
    real azSqr = gAccelStats.lpfAccel[2] * gAccelStats.lpfAccel[2];
    real sumSqr = axSqr + aySqr + azSqr;
    axSqr /= sumSqr;
    aySqr /= sumSqr;
    azSqr /= sumSqr;
    // pitch var
    real mult = 1.0f - axSqr;
    if (mult < 1.0e-2)
    {
        mult = 1.0e-2f;
    }
    mult = 1.0f / mult;  // mult = 1 / (1-ax^2) = 1 / (ay^2 + az^2)
    gKalmanFilter.R[STATE_PITCH] = mult * totalAccelVar[X_AXIS];
    //  roll var
    mult *= mult;   // multi = 1 / (ay^2 + az^2)^2
    gKalmanFilter.R[STATE_ROLL] = mult * azSqr * azSqr * totalAccelVar[Y_AXIS] +
        mult * aySqr * totalAccelVar[Z_AXIS];

    // additional R
    gKalmanFilter.R[STATE_ROLL] += additionalR;
    gKalmanFilter.R[STATE_PITCH] += additionalR;

    // limit R
    real maxR = 400.0f * Rnom;
    if (gKalmanFilter.R[STATE_ROLL] > maxR)
    {
        gKalmanFilter.R[STATE_ROLL] = maxR;
    }
    if (gKalmanFilter.R[STATE_PITCH] > maxR)
    {
        gKalmanFilter.R[STATE_PITCH] = maxR;
    }

    /* Yaw
     * ------ From NovAtel's description of BESTVEL: ------
     * Velocity (speed and direction) calculations are computed from either
     * Doppler or carrier phase measurements rather than from pseudorange
     * measurements. Typical speed accuracies are around 0.03m/s (0.07 mph,
     * 0.06 knots).
     *
     * Direction accuracy is derived as a function of the vehicle speed. A
     * simple approach would be to assume a worst case 0.03 m/s cross-track
     * velocity that would yield a direction error function something like:
     *
     * d (speed) = tan-1(0.03/speed)
     *
     * For example, if you are flying in an airplane at a speed of 120 knots
     * or 62 m/s, the approximate directional error will be:
     *
     * tan-1 (0.03/62) = 0.03 degrees
     *
     * Consider another example applicable to hiking at an average walking
     * speed of 3 knots or 1.5 m/s. Using the same error function yields a
     * direction error of about 1.15 degrees.
     *
     * You can see from both examples that a faster vehicle speed allows for a
     * more accurate heading indication. As the vehicle slows down, the
     * velocity information becomes less and less accurate. If the vehicle is
     * stopped, a GNSS receiver still outputs some kind of movement at speeds
     * between 0 and 0.5 m/s in random and changing directions. This
     * represents the noise and error of the static position.

     * ----- Yaw -----
     * CHANGED TO SWITCH BETWEEN GPS AND MAG UPDATES
     */
    if ( useGpsHeading )
    {
        float temp = (float)atan( 0.05 / gEKFInputData.rawGroundSpeed );
        gKalmanFilter.R[STATE_YAW] = temp * temp;
        if (gAlgoStatus.bit.turnSwitch)
        {
            gKalmanFilter.R[STATE_YAW] *= 10.0;
        }
    }
    else if ( magUsedInAlgorithm() && gAlgorithm.state <= LOW_GAIN_AHRS )
    {
        // todo: need to further distinguish between if mag is used
        // MAGNETOMETERS
        if( (gAlgorithm.state == HIGH_GAIN_AHRS) ||
            (gAlgorithm.linAccelSwitch == TRUE) )
        {
            // --- High-Gain ---
            gKalmanFilter.R[STATE_YAW] = (real)1.0e-2;  // jun4
        } else {
            // --- Low-Gain ---
            gKalmanFilter.R[STATE_YAW]   = (real)1.0e-1; // v14.6 values
        }

        /* For 'large' roll/pitch angles, increase R-yaw to decrease the effect
         * of update due to potential uncompensated z-axis magnetometer
         * readings from affecting the yaw-update.
         */
        if( ( gKalmanFilter.eulerAngles[ROLL]  > TEN_DEGREES_IN_RAD ) ||
            ( gKalmanFilter.eulerAngles[PITCH] > TEN_DEGREES_IN_RAD ) )
        {
            gKalmanFilter.R[STATE_YAW] = (real)0.2;
        }
    }
    else
    {
        gKalmanFilter.R[STATE_YAW] = (real)0.1;
    }
}

// 
void _GenerateObservationCovariance_INS(void)
{
    // Only need to compute certain elements of R once
    static BOOL initR = TRUE;
    if (initR) 
    {
        initR = FALSE;

        gKalmanFilter.R[STATE_RX] = (real)R_VALS_GPS_POS_X;
        gKalmanFilter.R[STATE_RY] = gKalmanFilter.R[STATE_RX];
        gKalmanFilter.R[STATE_RZ] = gKalmanFilter.R[STATE_RX];

        gKalmanFilter.R[STATE_VX] = (real)R_VALS_GPS_VEL_X;
        gKalmanFilter.R[STATE_VY] = gKalmanFilter.R[STATE_VX];
        gKalmanFilter.R[STATE_VZ] = gKalmanFilter.R[STATE_VX];
    }

    /* Use the GPS-provided horizontal and vertical accuracy values to populate
     * the covariance values.
     */
    gKalmanFilter.R[STATE_RX] = gEKFInputData.GPSHorizAcc * gEKFInputData.GPSHorizAcc;
    gKalmanFilter.R[STATE_RY] = gKalmanFilter.R[STATE_RX];
    gKalmanFilter.R[STATE_RZ] = gEKFInputData.GPSVertAcc * gEKFInputData.GPSVertAcc;

    /* Scale the best velocity error by HDOP then multiply by the z-axis angular
     * rate PLUS one (to prevent the number from being zero) so the velocity
     * update during high-rate turns is reduced.
     */
    float temp = (real)0.0625 * gEKFInputData.HDOP;  // 0.0625 = 0.05 / 0.8
    real absFilteredYawRate = (real)fabs(gAlgorithm.filteredYawRate);
    if (absFilteredYawRate > TEN_DEGREES_IN_RAD)
    {
        temp *= (1.0f + absFilteredYawRate);
    }
    gKalmanFilter.R[STATE_VX] = temp;// *((real)1.0 + fabs(gAlgorithm.filteredYawRate) * (real)RAD_TO_DEG);
    gKalmanFilter.R[STATE_VX] = gKalmanFilter.R[STATE_VX] * gKalmanFilter.R[STATE_VX];
    gKalmanFilter.R[STATE_VY] = gKalmanFilter.R[STATE_VX];

    // z-axis velocity isn't really a function of yaw-rate and hdop
    gKalmanFilter.R[STATE_VZ] = (float)(0.1 * 0.1);
}


//
uint8_t rowNum, colNum, multIndex;

real S_3x3[3][3], SInverse_3x3[3][3];
real PxHTranspose[ROWS_IN_P][ROWS_IN_H];
real KxH[NUMBER_OF_EKF_STATES][COLS_IN_H] = {{ 0.0 }};
real deltaP_tmp[ROWS_IN_P][COLS_IN_P];

void Update_Att(void)
{
    // which state is updated in Update_Att()
    uint8_t updatedStatesAtt[16] = { 1, 1, 1,           // Positions are not updated
                                     1, 1, 1,           // Velocities are not updated
                                     1, 1, 1, 1,        // Quaternions are updated
                                     1, 1, 1,           // Gyro biases are updated
                                     1, 1, 1};          // Accel biases are not upated

    /* Calculate the elements in the H and R matrices
     *  Matrix sizes for an Euler-angle based AHRS solution:
     */
    _GenerateObservationJacobian_AHRS();     // gKF.H: 3x16
    _GenerateObservationCovariance_AHRS();   // gKF.R: 3x3
    // If neither mag or GPS headig is available, update measuremnt matrix H to 2x16
    if (gKalmanFilter.R[STATE_YAW] > 0.9)
    {
        for (colNum = 0; colNum < COLS_IN_H; colNum++)
        {
            gKalmanFilter.H[2][colNum] = 0.0;
        }
    }
    /* This solution consists of an integrated roll/pitch/yaw solution
     * S = H*P*HTrans + R (However the matrix math can be simplified since
     *                     H is very sparse!  P is fully populated)
     * Update P from the P, H, and R matrices: P = HxPxHTranspose + R
     */
    // 1) PxHTranspose is computed first
    memset(PxHTranspose, 0, sizeof(PxHTranspose));
    for (rowNum = 0; rowNum < ROWS_IN_P; rowNum++) 
    {
        for (colNum = 0; colNum < ROWS_IN_H; colNum++) 
        {
            for (multIndex = RLE_H[colNum][0]; multIndex <= RLE_H[colNum][1]; multIndex++) 
            {
                PxHTranspose[rowNum][colNum] = PxHTranspose[rowNum][colNum] +
                    gKalmanFilter.P[rowNum][multIndex] * gKalmanFilter.H[colNum][multIndex];
            }
        }
    }

    /* HPH' is symmetric so only need to multiply one half and reflect the values
     * across the diagonal. S_3x3 is to hold values of HPH'.
     */
    for (rowNum = 0; rowNum < ROWS_IN_H; rowNum++) 
    {
        for (colNum = rowNum; colNum < ROWS_IN_H; colNum++) 
        {
            S_3x3[rowNum][colNum] = 0.0;
            for (multIndex = RLE_H[rowNum][0]; multIndex <= RLE_H[rowNum][1]; multIndex++) 
            {
                S_3x3[rowNum][colNum] = S_3x3[rowNum][colNum] +
                    gKalmanFilter.H[rowNum][multIndex] * PxHTranspose[multIndex][colNum];
            }
            S_3x3[colNum][rowNum] = S_3x3[rowNum][colNum];
        }
    }

    // S = HxPxHTranspose + R (rows 7:10 and cols 7:10 of P PLUS diagonal of R)
    S_3x3[ROLL][ROLL]   +=  gKalmanFilter.R[STATE_ROLL];
    S_3x3[PITCH][PITCH] += gKalmanFilter.R[STATE_PITCH];
    S_3x3[YAW][YAW]     +=  gKalmanFilter.R[STATE_YAW];

    // Invert the S-Matrix (replace with sequential update)
    matrixInverse_3x3(&S_3x3[0][0], &SInverse_3x3[0][0]);

    // Compute the Kalman gain: K = P*HTrans*SInv
    AxB( &PxHTranspose[0][0],
         &SInverse_3x3[0][0],
         ROWS_IN_P, ROWS_IN_H, ROWS_IN_H,
         &gKalmanFilter.K[0][0] );

    // force unupdated terms in K to be 0
    for (rowNum = STATE_RX; rowNum <= STATE_ABZ; rowNum++)
    {
        if (!updatedStatesAtt[rowNum])
        {
            for (colNum = 0; colNum < 3; colNum++)
            {
                gKalmanFilter.K[rowNum][colNum] = 0.0;
            }
        }
    }

    /* Compute attitude-quaternion updates: Dx = K*nu
     * NOTE: Can access nu in the elements that the attitude error is stored BUT the
     * value of ROWS_IN_H must be correct or the multiplication will be wrong
     */
    AxV( &gKalmanFilter.K[0][0],
         &gKalmanFilter.nu[STATE_ROLL],
         NUMBER_OF_EKF_STATES, ROWS_IN_H,
         &gKalmanFilter.stateUpdate[0] );

    // Update states based on computed deltas
    // --- attitude quaternions (q = q + Dq) ---
    gKalmanFilter.quaternion[Q0] = gKalmanFilter.quaternion[Q0] + gKalmanFilter.stateUpdate[STATE_Q0];
    gKalmanFilter.quaternion[Q1] = gKalmanFilter.quaternion[Q1] + gKalmanFilter.stateUpdate[STATE_Q1];
    gKalmanFilter.quaternion[Q2] = gKalmanFilter.quaternion[Q2] + gKalmanFilter.stateUpdate[STATE_Q2];
    gKalmanFilter.quaternion[Q3] = gKalmanFilter.quaternion[Q3] + gKalmanFilter.stateUpdate[STATE_Q3];

    // Normalize q
    QuatNormalize(&gKalmanFilter.quaternion[0]);
    
    // --- Angular-rate bias (wBias = wBias = DwBias) ---
    //     If magnetometers are not used then set the rate bias to zero???
    gKalmanFilter.rateBias_B[X_AXIS] = gKalmanFilter.rateBias_B[X_AXIS] + gKalmanFilter.stateUpdate[STATE_WBX];
    gKalmanFilter.rateBias_B[Y_AXIS] = gKalmanFilter.rateBias_B[Y_AXIS] + gKalmanFilter.stateUpdate[STATE_WBY];
    gKalmanFilter.rateBias_B[Z_AXIS] = gKalmanFilter.rateBias_B[Z_AXIS] + gKalmanFilter.stateUpdate[STATE_WBZ];

    /* Update covariance: P = P + DP = P - K*H*P
     * KxH = gKF.K * gKF.H;
     */
    /* 2) Use gKalmanFilter.P as a temporary variable to hold FxPxFTranspose
     * to reduce the number of "large" variables on the heap
     */
    memset(KxH, 0, sizeof(KxH));
    for (rowNum = 0; rowNum < ROWS_IN_K; rowNum++) 
    {
        for (colNum = RLE_KxH[rowNum][0]; colNum <= RLE_KxH[rowNum][1]; colNum++)
        {
            for (multIndex = 0; multIndex < ROWS_IN_H; multIndex++)
            {
                KxH[rowNum][colNum] = KxH[rowNum][colNum] +
                    gKalmanFilter.K[rowNum][multIndex] * gKalmanFilter.H[multIndex][colNum];
            }
        }
    }

    // deltaP = KxH * gKF.P;
    memset(deltaP_tmp, 0, sizeof(deltaP_tmp));
    /* deltaP is symmetric so only need to multiply one half and reflect the values
     * across the diagonal
     */
#if 0
    for (rowNum = 0; rowNum < ROWS_IN_K; rowNum++) 
    {
        for (colNum = rowNum; colNum < COLS_IN_P; colNum++) 
        {
            for (multIndex = RLE_KxH[rowNum][0]; multIndex <= RLE_KxH[rowNum][1]; multIndex++)
            {
                deltaP_tmp[rowNum][colNum] = deltaP_tmp[rowNum][colNum] +
                    KxH[rowNum][multIndex] * gKalmanFilter.P[multIndex][colNum];
            }
            deltaP_tmp[colNum][rowNum] = deltaP_tmp[rowNum][colNum];
        }
    }
#else
    for (rowNum = 0; rowNum <= STATE_ABZ; rowNum++)
    {
        if (!updatedStatesAtt[rowNum])
        {
            continue;
        }
        for (colNum = rowNum; colNum <= STATE_ABZ; colNum++)
        {
            if (!updatedStatesAtt[colNum])
            {
                continue;
            }
            for (multIndex = RLE_KxH[rowNum][0]; multIndex <= RLE_KxH[rowNum][1]; multIndex++) 
            {
                if (!updatedStatesAtt[multIndex])
                {
                    continue;
                }
                deltaP_tmp[rowNum][colNum] = deltaP_tmp[rowNum][colNum] +
                    KxH[rowNum][multIndex] * gKalmanFilter.P[multIndex][colNum];
            }
            deltaP_tmp[colNum][rowNum] = deltaP_tmp[rowNum][colNum];
        }
    }
#endif
    /* P is symmetric so only need to multiply one half and reflect the values
     * across the diagonal
     */
    for (rowNum = 0; rowNum < ROWS_IN_P; rowNum++) 
    {
        for (colNum = rowNum; colNum < COLS_IN_P; colNum++) 
        {
            gKalmanFilter.P[rowNum][colNum] = gKalmanFilter.P[rowNum][colNum] -
                                              deltaP_tmp[rowNum][colNum];
            gKalmanFilter.P[colNum][rowNum] = gKalmanFilter.P[rowNum][colNum];
        }
    }
}


/* The position update only allows us to update the position and velocity states (along
 * with Pr and Pv).  Want to verify this...
 */
void Update_Pos(void)
{
    // which state is updated in Update_Att()
    uint8_t updatedStatesPos[16] = { 1, 1, 1,           // Positions are updated
                                     1, 1, 1,           // Velocities are updated
                                     1, 1, 1, 1,        // Quaternions are NOT updated
                                     1, 1, 1,           // Gyro biases are NOT updated
                                     1, 1, 1 };         // Accel biases are NOT upated


    /* S1 = H1*gKF.P*H1' + R1;   Top 3 rows of the first 3 cols of P + R
     * K1 = gKF.P*H1'*inv(S1);   ( first 3 cols of P ) * S1Inverse
     * P1 = (eye(16) - K1*H1) * gKF.P;
     * H2 = [I3x3 0_3x3 0_3x4 0_3x3 0_3x3]
     */

    // S1 = H1*gKF.P*H1' + R1;
    S_3x3[0][0] = gKalmanFilter.P[STATE_RX][STATE_RX] + gKalmanFilter.R[STATE_RX];
    S_3x3[0][1] = gKalmanFilter.P[STATE_RX][STATE_RY];
    S_3x3[0][2] = gKalmanFilter.P[STATE_RX][STATE_RZ];

    S_3x3[1][0] = gKalmanFilter.P[STATE_RY][STATE_RX];
    S_3x3[1][1] = gKalmanFilter.P[STATE_RY][STATE_RY] + gKalmanFilter.R[STATE_RY];
    S_3x3[1][2] = gKalmanFilter.P[STATE_RY][STATE_RZ];

    S_3x3[2][0] = gKalmanFilter.P[STATE_RZ][STATE_RX];
    S_3x3[2][1] = gKalmanFilter.P[STATE_RZ][STATE_RY];
    S_3x3[2][2] = gKalmanFilter.P[STATE_RZ][STATE_RZ] + gKalmanFilter.R[STATE_RZ];

    // S1_Inverse
    matrixInverse_3x3(&S_3x3[0][0], &SInverse_3x3[0][0]);

    // Compute K1 = ( gKF.P*H1' ) * S1Inverse = ( first 3 cols of P ) * S1Inverse
    for (rowNum = 0; rowNum < NUMBER_OF_EKF_STATES; rowNum++)
    {
        for (colNum = X_AXIS; colNum <= Z_AXIS; colNum++)
        {
            gKalmanFilter.K[rowNum][colNum] = 0.0;
            // H is sparse so only the columns of P associated with the position states are used
            //   in the calculation
            for (multIndex = STATE_RX; multIndex <= STATE_RZ; multIndex++)
            {
                gKalmanFilter.K[rowNum][colNum] = gKalmanFilter.K[rowNum][colNum] +
                                        gKalmanFilter.P[rowNum][multIndex] * SInverse_3x3[multIndex - STATE_RX][colNum];
            }
        }
    }

    // force uncorrected terms in K to be 0
    for (rowNum = STATE_RX; rowNum <= STATE_ABZ; rowNum++)
    {
        if (!updatedStatesPos[rowNum])
        {
            for (colNum = 0; colNum < 3; colNum++)
            {
                gKalmanFilter.K[rowNum][colNum] = 0.0;
            }
        }
    }
    // Compute the intermediate state update, stateUpdate
    AxB(&gKalmanFilter.K[0][0], &gKalmanFilter.nu[STATE_RX], NUMBER_OF_EKF_STATES, 3, 1, &gKalmanFilter.stateUpdate[0]);

    memset(deltaP_tmp, 0, sizeof(deltaP_tmp));
    // Update the intermediate covariance estimate
    for (rowNum = 0; rowNum < NUMBER_OF_EKF_STATES; rowNum++) 
    {
        if (!updatedStatesPos[rowNum])
        {
            continue;
        }
        for (colNum = rowNum; colNum < NUMBER_OF_EKF_STATES; colNum++) 
        {
            if (!updatedStatesPos[colNum])
            {
                continue;
            }
            /* H is sparse so only the columns of P associated with the position states are used
             * in the calculation
             */
            for (multIndex = STATE_RX; multIndex <= STATE_RZ; multIndex++) 
            {
                if (!updatedStatesPos[multIndex])
                {
                    continue;
                }
                deltaP_tmp[rowNum][colNum] = deltaP_tmp[rowNum][colNum] +
                    gKalmanFilter.K[rowNum][multIndex] * gKalmanFilter.P[multIndex][colNum];
            }
            deltaP_tmp[colNum][rowNum] = deltaP_tmp[rowNum][colNum];
        }
    }

    AMinusB(&gKalmanFilter.P[0][0], &deltaP_tmp[0][0], NUMBER_OF_EKF_STATES, NUMBER_OF_EKF_STATES, &gKalmanFilter.P[0][0]);

    // Update states
    gKalmanFilter.Position_N[X_AXIS] = gKalmanFilter.Position_N[X_AXIS] + gKalmanFilter.stateUpdate[STATE_RX];
    gKalmanFilter.Position_N[Y_AXIS] = gKalmanFilter.Position_N[Y_AXIS] + gKalmanFilter.stateUpdate[STATE_RY];
    gKalmanFilter.Position_N[Z_AXIS] = gKalmanFilter.Position_N[Z_AXIS] + gKalmanFilter.stateUpdate[STATE_RZ];

    gKalmanFilter.Velocity_N[X_AXIS] = gKalmanFilter.Velocity_N[X_AXIS] + gKalmanFilter.stateUpdate[STATE_VX];
    gKalmanFilter.Velocity_N[Y_AXIS] = gKalmanFilter.Velocity_N[Y_AXIS] + gKalmanFilter.stateUpdate[STATE_VY];
    gKalmanFilter.Velocity_N[Z_AXIS] = gKalmanFilter.Velocity_N[Z_AXIS] + gKalmanFilter.stateUpdate[STATE_VZ];

    //gKalmanFilter.quaternion[Q0] = gKalmanFilter.quaternion[Q0] + gKalmanFilter.stateUpdate[STATE_Q0];
    //gKalmanFilter.quaternion[Q1] = gKalmanFilter.quaternion[Q1] + gKalmanFilter.stateUpdate[STATE_Q1];
    //gKalmanFilter.quaternion[Q2] = gKalmanFilter.quaternion[Q2] + gKalmanFilter.stateUpdate[STATE_Q2];
    //gKalmanFilter.quaternion[Q3] = gKalmanFilter.quaternion[Q3] + gKalmanFilter.stateUpdate[STATE_Q3];
    //
    //// Normalize quaternion and force q0 to be positive
    //QuatNormalize(gKalmanFilter.quaternion);
    //
    //gKalmanFilter.rateBias_B[X_AXIS] = gKalmanFilter.rateBias_B[X_AXIS] + gKalmanFilter.stateUpdate[STATE_WBX];
    //gKalmanFilter.rateBias_B[Y_AXIS] = gKalmanFilter.rateBias_B[Y_AXIS] + gKalmanFilter.stateUpdate[STATE_WBY];
    //gKalmanFilter.rateBias_B[Z_AXIS] = gKalmanFilter.rateBias_B[Z_AXIS] + gKalmanFilter.stateUpdate[STATE_WBZ];
    //
    //gKalmanFilter.accelBias_B[X_AXIS] = gKalmanFilter.accelBias_B[X_AXIS] + gKalmanFilter.stateUpdate[STATE_ABX];
    //gKalmanFilter.accelBias_B[Y_AXIS] = gKalmanFilter.accelBias_B[Y_AXIS] + gKalmanFilter.stateUpdate[STATE_ABY];
    //gKalmanFilter.accelBias_B[Z_AXIS] = gKalmanFilter.accelBias_B[Z_AXIS] + gKalmanFilter.stateUpdate[STATE_ABZ];
}


/* The velocity update only allows us to update the velocity, attitude, and acceleration-
 * bias states (along with Pv, Pq, and Pab).  Wwant to verify this...
 */
void Update_Vel(void)
{
    // which state is updated in Update_Att()
    uint8_t updatedStatesVel[16] = { 1, 1, 1,           // Positions are NOT updated
                                     1, 1, 1,           // Velocities are updated
                                     1, 1, 1, 1,        // Quaternions are updated
                                     1, 1, 1,           // Gyro biases are NOT updated
                                     1, 1, 1 };         // Accel biases are upated

    /* S2 = H2*P1*H2' + R2; (4th, 5th, and 6th rows of the 4th, 5th, and 6th cols of P1)
     * K2 = P1*H2'*inv(S2);
     * P2 = (eye(16) - K2*H2) * P1;
     * H2 = [0_3x3 I3x3 0_3x4 0_3x3 0_3x3]
     */

    // S2 = H2*P1*H2' + R2;
    S_3x3[0][0] = gKalmanFilter.P[STATE_VX][STATE_VX] + gKalmanFilter.R[STATE_VX];
    S_3x3[0][1] = gKalmanFilter.P[STATE_VX][STATE_VY];
    S_3x3[0][2] = gKalmanFilter.P[STATE_VX][STATE_VZ];

    S_3x3[1][0] = gKalmanFilter.P[STATE_VY][STATE_VX];
    S_3x3[1][1] = gKalmanFilter.P[STATE_VY][STATE_VY] + gKalmanFilter.R[STATE_VY];
    S_3x3[1][2] = gKalmanFilter.P[STATE_VY][STATE_VZ];

    S_3x3[2][0] = gKalmanFilter.P[STATE_VZ][STATE_VX];
    S_3x3[2][1] = gKalmanFilter.P[STATE_VZ][STATE_VY];
    S_3x3[2][2] = gKalmanFilter.P[STATE_VZ][STATE_VZ] + gKalmanFilter.R[STATE_VZ];

    // S2_Inverse
    matrixInverse_3x3(&S_3x3[0][0], &SInverse_3x3[0][0]);

    // Compute K2 = ( P1*H2' ) * S2Inverse = ( 4th, 5th, and 6th cols of P1 ) * S2Inverse
    for (rowNum = 0; rowNum < NUMBER_OF_EKF_STATES; rowNum++) 
    {
        for (colNum = X_AXIS; colNum <= Z_AXIS; colNum++) 
        {
            gKalmanFilter.K[rowNum][colNum] = 0.0;
            /* H is sparse so only the columns of P associated with the velocity states are used
             * in the calculation
             */
            for (multIndex = STATE_VX; multIndex <= STATE_VZ; multIndex++) 
            {
                gKalmanFilter.K[rowNum][colNum] = gKalmanFilter.K[rowNum][colNum] +
                                        gKalmanFilter.P[rowNum][multIndex] * SInverse_3x3[multIndex - STATE_VX][colNum];
            }
        }
    }

    // force uncorrected terms in K to be 0
    for (rowNum = STATE_RX; rowNum <= STATE_ABZ; rowNum++)
    {
        if (!updatedStatesVel[rowNum])
        {
            for (colNum = 0; colNum < 3; colNum++)
            {
                gKalmanFilter.K[rowNum][colNum] = 0.0;
            }
        }
    }
    // Compute the intermediate state update
    AxB(&gKalmanFilter.K[0][0], &gKalmanFilter.nu[STATE_VX], NUMBER_OF_EKF_STATES, 3, 1, &gKalmanFilter.stateUpdate[0]);

    memset(deltaP_tmp, 0, sizeof(deltaP_tmp));
    // Update the intermediate covariance estimate
    for (rowNum = 0; rowNum < NUMBER_OF_EKF_STATES; rowNum++) 
    {
        if (!updatedStatesVel[rowNum])
        {
            continue;
        }
        for (colNum = rowNum; colNum < NUMBER_OF_EKF_STATES; colNum++) 
        {
            if (!updatedStatesVel[colNum])
            {
                continue;
            }
            /* H is sparse so only the columns of P associated with the velocity states are used
             * in the calculation
             */
            for (multIndex = STATE_VX; multIndex <= STATE_VZ; multIndex++) 
            {
                deltaP_tmp[rowNum][colNum] = deltaP_tmp[rowNum][colNum] +
                    gKalmanFilter.K[rowNum][multIndex - STATE_VX] * gKalmanFilter.P[multIndex][colNum];
            }
            deltaP_tmp[colNum][rowNum] = deltaP_tmp[rowNum][colNum];
        }
    }

    // P2 = P2 - KxHxP2
    AMinusB(&gKalmanFilter.P[0][0], &deltaP_tmp[0][0], NUMBER_OF_EKF_STATES, NUMBER_OF_EKF_STATES, &gKalmanFilter.P[0][0]);
    // ++++++++++++++++++++++ END OF VELOCITY ++++++++++++++++++++++

    // Update states
    //gKalmanFilter.Position_N[X_AXIS] = gKalmanFilter.Position_N[X_AXIS] + gKalmanFilter.stateUpdate[STATE_RX];
    //gKalmanFilter.Position_N[Y_AXIS] = gKalmanFilter.Position_N[Y_AXIS] + gKalmanFilter.stateUpdate[STATE_RY];
    //gKalmanFilter.Position_N[Z_AXIS] = gKalmanFilter.Position_N[Z_AXIS] + gKalmanFilter.stateUpdate[STATE_RZ];

    gKalmanFilter.Velocity_N[X_AXIS] = gKalmanFilter.Velocity_N[X_AXIS] + gKalmanFilter.stateUpdate[STATE_VX];
    gKalmanFilter.Velocity_N[Y_AXIS] = gKalmanFilter.Velocity_N[Y_AXIS] + gKalmanFilter.stateUpdate[STATE_VY];
    gKalmanFilter.Velocity_N[Z_AXIS] = gKalmanFilter.Velocity_N[Z_AXIS] + gKalmanFilter.stateUpdate[STATE_VZ];

    gKalmanFilter.quaternion[Q0] = gKalmanFilter.quaternion[Q0] + gKalmanFilter.stateUpdate[STATE_Q0];
    gKalmanFilter.quaternion[Q1] = gKalmanFilter.quaternion[Q1] + gKalmanFilter.stateUpdate[STATE_Q1];
    gKalmanFilter.quaternion[Q2] = gKalmanFilter.quaternion[Q2] + gKalmanFilter.stateUpdate[STATE_Q2];
    gKalmanFilter.quaternion[Q3] = gKalmanFilter.quaternion[Q3] + gKalmanFilter.stateUpdate[STATE_Q3];

    // Normalize quaternion and force q0 to be positive
    QuatNormalize(gKalmanFilter.quaternion);

    //gKalmanFilter.rateBias_B[X_AXIS] = gKalmanFilter.rateBias_B[X_AXIS] + gKalmanFilter.stateUpdate[STATE_WBX];
    //gKalmanFilter.rateBias_B[Y_AXIS] = gKalmanFilter.rateBias_B[Y_AXIS] + gKalmanFilter.stateUpdate[STATE_WBY];
    //gKalmanFilter.rateBias_B[Z_AXIS] = gKalmanFilter.rateBias_B[Z_AXIS] + gKalmanFilter.stateUpdate[STATE_WBZ];

    gKalmanFilter.accelBias_B[X_AXIS] = gKalmanFilter.accelBias_B[X_AXIS] + gKalmanFilter.stateUpdate[STATE_ABX];
    gKalmanFilter.accelBias_B[Y_AXIS] = gKalmanFilter.accelBias_B[Y_AXIS] + gKalmanFilter.stateUpdate[STATE_ABY];
    gKalmanFilter.accelBias_B[Z_AXIS] = gKalmanFilter.accelBias_B[Z_AXIS] + gKalmanFilter.stateUpdate[STATE_ABZ];
}

static void Update_GPS(void)
{
    // Sync the algorithm itow to the GPS value
    gAlgorithm.itow = gEKFInputData.itow;

    // Resync timer
    timer.tenHertzCntr = 0;
    timer.subFrameCntr = 0;

    // Debugging counter: updateCntr[0] should be about 9x less than updateCntr[1]
    updateCntr[0] = updateCntr[0] + 1;

    // reset the "last good reading" time
    gAlgorithm.timeOfLastGoodGPSReading = gEKFInputData.itow;

    // GPS heading valid?
    useGpsHeading = (gEKFInputData.rawGroundSpeed >= LIMIT_MIN_GPS_VELOCITY_HEADING);

    /* Extract what's common between the following function and the
     * routines below so we aren't repeating calculations
     */
    LLA_To_Base(&gEKFInputData.llaRad[0],
        &gAlgorithm.rGPS0_E[0],
        &gAlgorithm.rGPS_N[0],
        &gAlgorithm.R_NinE[0][0],
        &gAlgorithm.rGPS_E[0]);

    /* Lever-arm is antenna position w.r.t to IMU in body. rGPS_N is antenna positive
     * w.r.t to initial point in NED. IMU positive w.r.t initial point is
     * rGPS_N - R_b_to_N * lever-arm
     */
    float leverArmN[3];
    leverArmN[0] = gKalmanFilter.R_BinN[0][0] * gAlgorithm.leverArmB[0] +
        gKalmanFilter.R_BinN[0][1] * gAlgorithm.leverArmB[1] +
        gKalmanFilter.R_BinN[0][2] * gAlgorithm.leverArmB[2];
    leverArmN[1] = gKalmanFilter.R_BinN[1][0] * gAlgorithm.leverArmB[0] +
        gKalmanFilter.R_BinN[1][1] * gAlgorithm.leverArmB[1] +
        gKalmanFilter.R_BinN[1][2] * gAlgorithm.leverArmB[2];
    leverArmN[2] = gKalmanFilter.R_BinN[2][0] * gAlgorithm.leverArmB[0] +
        gKalmanFilter.R_BinN[2][1] * gAlgorithm.leverArmB[1] +
        gKalmanFilter.R_BinN[2][2] * gAlgorithm.leverArmB[2];
    gAlgorithm.rGPS_N[0] -= leverArmN[0];
    gAlgorithm.rGPS_N[1] -= leverArmN[1];
    gAlgorithm.rGPS_N[2] -= leverArmN[2];
    /* This Sequential-Filter (three-stage approach) is nearly as
     * good as the full implementation -- we also can split it
     * across multiple iterations to not exceed 10 ms execution on
     * the embedded 380
     */
     /* Compute the system error: z = meas, h = pred = q, nu - z - h
      * Do this at the same time even if the update is spread across time-steps
      */
    ComputeSystemInnovation_Pos();
    ComputeSystemInnovation_Vel();
    ComputeSystemInnovation_Att();

    // Calculate the R-values for the INS measurements
    _GenerateObservationCovariance_INS();

    Update_Pos();

    Update_Vel();

    // Initialize heading. If getting initial heading at this step, do not update att
    if (gAlgorithm.gnssHeadingFirstTime)
    {
        if (InitializeGnssHeading())
        {
            /* This heading measurement is used to initialize heading, and should not be
             * used to update heading.
             */
            useGpsHeading = FALSE;
        }
    }
}

static void Update_PseudoVel(void)
{
    //real h[2][16];    // 2nd and 3rd columns of rb2n
    //real hpht[2][2];  // s
    real s_2x2[2][2];
    real invs_2x2[2][2];
    real k[16][2];
    real rb2n[3][3];
    QuaternionToR321(gKalmanFilter.quaternion, &rb2n[0][0]);

    // measurement cov
    real r[2] = { 1.0e-4, 1.0e-4 };

    // compute innovation: 0 - Rn2b * v_ned
    // lateral vel
    gKalmanFilter.nu[STATE_VY] = 
        -rb2n[0][1] * gKalmanFilter.Velocity_N[0] 
        -rb2n[1][1] * gKalmanFilter.Velocity_N[1]
        -rb2n[2][1] * gKalmanFilter.Velocity_N[2];
    // vertical vel
    gKalmanFilter.nu[STATE_VZ] =
        -rb2n[0][2] * gKalmanFilter.Velocity_N[0]
        -rb2n[1][2] * gKalmanFilter.Velocity_N[1]
        -rb2n[2][2] * gKalmanFilter.Velocity_N[2];
    gKalmanFilter.nu[0] = _LimitValue(gKalmanFilter.nu[0], gAlgorithm.Limit.Innov.velocityError);
    gKalmanFilter.nu[1] = _LimitValue(gKalmanFilter.nu[1], gAlgorithm.Limit.Innov.velocityError);

    // p*H'
    memset(PxHTranspose, 0, sizeof(PxHTranspose));
    for (rowNum = 0; rowNum < NUMBER_OF_EKF_STATES; rowNum++)
    {
        for (colNum = 0; colNum < 2; colNum++)
        {
            PxHTranspose[rowNum][colNum] =
                gKalmanFilter.P[rowNum][3] * rb2n[0][colNum+1] +
                gKalmanFilter.P[rowNum][4] * rb2n[1][colNum+1] +
                gKalmanFilter.P[rowNum][5] * rb2n[2][colNum+1];
        }
    }
    
    // s = H*P*H' + R
    for (rowNum = 0; rowNum < 2; rowNum++)
    {
        for (colNum = 0; colNum < 2; colNum++)
        {
            s_2x2[rowNum][colNum] =
                rb2n[0][rowNum + 1] * PxHTranspose[3][colNum] +
                rb2n[1][rowNum + 1] * PxHTranspose[4][colNum] +
                rb2n[2][rowNum + 1] * PxHTranspose[5][colNum];
        }
    }
    s_2x2[0][0] += r[0];
    s_2x2[1][1] += r[1];
    matrixInverse_2x2(&s_2x2[0][0], &invs_2x2[0][0]);

    // K = P*H' * inv(H*P*H' + R)
    // Compute the Kalman gain: K = P*HTrans*SInv
    AxB(&PxHTranspose[0][0],
        &invs_2x2[0][0],
        ROWS_IN_P, 2, 2,
        &k[0][0]);

    // dx = k * nu
    AxV(&k[0][0],
        &gKalmanFilter.nu[STATE_VY],
        NUMBER_OF_EKF_STATES, 2,
        &gKalmanFilter.stateUpdate[0]);
}

/* Conversion from turn-rate threshold (values loaded into gConfiguration) to
 * decimal value in [rad/sec]:
 *
 *   thresh_rad = ( 10.0 * pi/180 );   % 0.1745
 *   thresh_counts = floor( thresh_rad * ( 2^16 / (2*pi) ) );   % 1820
 *   thresh_rad = thresh_counts * ( 2*pi / 2^16 )   % 1820 * (2*pi) / 2^16 = 0.1745
 */
real TILT_YAW_SWITCH_GAIN = (real)0.05;

// TurnSwitch.m
static void _TurnSwitch(void)
{
    static real minSwitch = (real)0.0, maxSwitch = (real)0.0;
    static real turnSwitchThresholdPast = (real)0.0;
    static real linInterpSF;

    real absYawRate;
    real turnSwitchScaleFactor;

    // gKF.filteredYawRate (calculated in the prediction stage)
    absYawRate = (real)fabs(gAlgorithm.filteredYawRate);

    // In case the user changes the TST during operation
    if (gAlgorithm.turnSwitchThreshold != turnSwitchThresholdPast)
    {
        turnSwitchThresholdPast = gAlgorithm.turnSwitchThreshold;

        // Example conversion: ( 1820*12868 / 2^27 ) * ( 180/pi )
        minSwitch = gAlgorithm.turnSwitchThreshold * (real)(DEG_TO_RAD);   // angle in radians
        maxSwitch = (real)2.0 * minSwitch;   // angle in radians

        linInterpSF = ((real)1.0 - TILT_YAW_SWITCH_GAIN) / (maxSwitch - minSwitch);
    }

    // Linear interpolation if the yawRate is above the specified threshold
    if ((gAlgorithm.state > HIGH_GAIN_AHRS) && (absYawRate > minSwitch))
    {
        gAlgoStatus.bit.turnSwitch = TRUE;
        //        std::cout << "TurnSwitch (INS): Activated\n";

        /* When the rate is below the maximum rate defined by turnSwitchThreshold,
         * then generate a scale-factor that is between ( 1.0 - G ) and 0.0 (based on absYawRate).
         * If it is above 'maxSwitch' then the SF is zero.
         */
        if (absYawRate < maxSwitch) 
        {
            turnSwitchScaleFactor = linInterpSF * (maxSwitch - absYawRate);
        } 
        else
        {
            // yaw-rate is above maxSwitch ==> no gain
            turnSwitchScaleFactor = (real)0.0;
        }

        // Specify the multiplier so it is between G and 1.0
        gKalmanFilter.turnSwitchMultiplier = TILT_YAW_SWITCH_GAIN + turnSwitchScaleFactor;
    }
    else
    {
        gAlgoStatus.bit.turnSwitch = FALSE;
        gKalmanFilter.turnSwitchMultiplier = (real)1.0;
    }
}


static real _UnwrapAttitudeError(real attitudeError)
{
    while (fabs(attitudeError) > PI)
    {
        if (attitudeError > PI)
        {
            attitudeError = attitudeError - (real)TWO_PI;
        } 
        else if (attitudeError < -PI)
        {
            attitudeError = attitudeError + (real)TWO_PI;
        }
    }

    return attitudeError;
}


static real _LimitValue(real value, real limit)
{
    if (value > limit) 
    {
        return limit;
    }
    else if (value < -limit)
    {
        return -limit;
    }

    return value;
}


/* Returns true when the system is ready to update (based on the timer values
 *   and the desired update rate)
 */
static BOOL _CheckForUpdateTrigger(uint8_t updateRate)
{
    //
    uint8_t oneHundredHzCntr;
    uint8_t gpsUpdate = 0;

    //
    switch( updateRate )
    {
        // ten-hertz update
        case 10:
            if( timer.subFrameCntr == 0 ) 
            {
                gpsUpdate = 1;
            }
            break;

        // twenty-hertz update
        case 20:
            if( timer.subFrameCntr == 0 || timer.subFrameCntr == 5 )
            {
                gpsUpdate = 1;
            }
            break;

        // twenty-hertz update
        case 25:
            oneHundredHzCntr = 10 * timer.tenHertzCntr + timer.subFrameCntr;

            // 
            if( oneHundredHzCntr ==  0 ||
                oneHundredHzCntr ==  4 ||
                oneHundredHzCntr ==  8 ||
                oneHundredHzCntr == 12 ||
                oneHundredHzCntr == 16 ||
                oneHundredHzCntr == 20 ||
                oneHundredHzCntr == 24 ||
                oneHundredHzCntr == 28 ||
                oneHundredHzCntr == 32 ||
                oneHundredHzCntr == 36 ||
                oneHundredHzCntr == 40 ||
                oneHundredHzCntr == 44 ||
                oneHundredHzCntr == 48 ||
                oneHundredHzCntr == 52 ||
                oneHundredHzCntr == 56 ||
                oneHundredHzCntr == 60 ||
                oneHundredHzCntr == 64 ||
                oneHundredHzCntr == 68 ||
                oneHundredHzCntr == 72 ||
                oneHundredHzCntr == 76 ||
                oneHundredHzCntr == 80 ||
                oneHundredHzCntr == 84 ||
                oneHundredHzCntr == 88 ||
                oneHundredHzCntr == 92 ||
                oneHundredHzCntr == 96 )
            {
                gpsUpdate = 1;
            }
            break;

        // fifty-hertz update
        case 50:
            if( timer.subFrameCntr == 0 ||
                timer.subFrameCntr == 2 ||
                timer.subFrameCntr == 4 ||
                timer.subFrameCntr == 6 ||
                timer.subFrameCntr == 8 )
            {
                gpsUpdate = 1;
            }
            break;

        // fifty-hertz update
        case 100:
            gpsUpdate = 1;
            break;
    }

    return gpsUpdate;
}


static int InitializeGnssHeading()
{
    /* enable declination correction, but the corrected magnetic yaw will not
     * be used if GPS is available.
     */
    gAlgorithm.applyDeclFlag = TRUE;

    if (useGpsHeading)// && gEKFInputData.rawGroundSpeed>1.0)
    {
        // Heading is initialized with GNSS
        gAlgorithm.gnssHeadingFirstTime = FALSE;

#ifdef INS_OFFLINE
        printf("gps heading: %f\n", gEKFInputData.trueCourse);
#else
#ifdef DISPLAY_DIAGNOSTIC_MSG
        DebugPrintString("gps heading");
        DebugPrintFloat(": ", gEKFInputData.trueCourse, 9);
        DebugPrintEndline();
#endif
#endif
        // initialize yaw angle with GPS heading
        gKalmanFilter.eulerAngles[YAW] = (real)(gEKFInputData.trueCourse * D2R);
        if (gKalmanFilter.eulerAngles[YAW] > PI)
        {
            gKalmanFilter.eulerAngles[YAW] -= (real)TWO_PI;
        }
        EulerAnglesToQuaternion(gKalmanFilter.eulerAngles, gKalmanFilter.quaternion);
        
        // reinitialize NED velocity
        gKalmanFilter.Velocity_N[X_AXIS] = (real)gEKFInputData.vNed[X_AXIS];
        gKalmanFilter.Velocity_N[Y_AXIS] = (real)gEKFInputData.vNed[Y_AXIS];
        gKalmanFilter.Velocity_N[Z_AXIS] = (real)gEKFInputData.vNed[Z_AXIS];
        
#if 1   // mod, DXG
        // reset quaternion and velocity terms in the P matrix
        int i, j;
        // vel row
        for (i = STATE_VX; i < STATE_VZ; i++)
        {
            for (j = 0; j < NUMBER_OF_EKF_STATES; j++)
            {
                if (i != j)
                {
                    gKalmanFilter.P[i][j] = 0;
                    gKalmanFilter.P[j][i] = 0;
                }
            }
        }
        // q0 row
        for (i = 0; i < NUMBER_OF_EKF_STATES; i++)
        {
            if (i != STATE_Q0)
            {
                gKalmanFilter.P[STATE_Q0][i] = 0;
                gKalmanFilter.P[i][STATE_Q0] = 0;
            }
        }
        // q3 row
        for (i = 0; i < NUMBER_OF_EKF_STATES; i++)
        {
            if (i != STATE_Q3)
            {
                gKalmanFilter.P[STATE_Q3][i] = 0;
                gKalmanFilter.P[i][STATE_Q3] = 0;
            }
        }

        // the initial covariance of the quaternion is estimated from ground speed.
        float temp = (float)atan(0.05 / gEKFInputData.rawGroundSpeed);
        temp *= temp;   // heading var
        if (gAlgoStatus.bit.turnSwitch)
        {
            temp *= 10.0;   // when rotating, heading var increases
        }
        temp /= 4.0;        // sin(heading/2) or cos(heading/2)
        float sinYawSqr = (real)sin(gKalmanFilter.eulerAngles[YAW] / 2.0f);
        sinYawSqr *= sinYawSqr;
        //  Assume roll and pitch are close to 0deg
        gKalmanFilter.P[STATE_Q0][STATE_Q0] = temp * sinYawSqr;
        gKalmanFilter.P[STATE_Q3][STATE_Q3] = temp * (1.0f - sinYawSqr);

        gKalmanFilter.P[STATE_VX][STATE_VX] = gKalmanFilter.R[STATE_VX];
        gKalmanFilter.P[STATE_VY][STATE_VY] = gKalmanFilter.R[STATE_VY];
        gKalmanFilter.P[STATE_VZ][STATE_VZ] = gKalmanFilter.R[STATE_VZ];

#if 0
        // reset velocity and quaternion terms in the P matrix
        real v2 = gEKFInputData.rawGroundSpeed * gEKFInputData.rawGroundSpeed;
        real v3by4 = 4.0 * v2 * gEKFInputData.rawGroundSpeed;
        real vn2 = gEKFInputData.vNed[0] * gEKFInputData.vNed[0];
        real q0q0 = gKalmanFilter.quaternion[0] * gKalmanFilter.quaternion[0];
        real q3q3 = gKalmanFilter.quaternion[3] * gKalmanFilter.quaternion[3];
        if (q0q0 < 1.0e-3)
        {
            q0q0 = 1.0e-3;
        }
        if (q3q3 < 1.0e-3)
        {
            q3q3 = 1.0e-3;
        }
        real multiplerQVn = (v2 - vn2) / v3by4;
        multiplerQVn *= multiplerQVn;
        real multiplerQVe = (gEKFInputData.vNed[0] * gEKFInputData.vNed[1]) / v3by4;
        multiplerQVe *= multiplerQVe;
        gKalmanFilter.P[STATE_VX][STATE_Q0] = multiplerQVn / q0q0 * gKalmanFilter.R[STATE_VX][STATE_VX];
        gKalmanFilter.P[STATE_VX][STATE_Q3] = multiplerQVn / q3q3 * gKalmanFilter.R[STATE_VX][STATE_VX];
        gKalmanFilter.P[STATE_VY][STATE_Q0] = multiplerQVe / q0q0 * gKalmanFilter.R[STATE_VY][STATE_VY];
        gKalmanFilter.P[STATE_VY][STATE_Q3] = multiplerQVe / q3q3 * gKalmanFilter.R[STATE_VY][STATE_VY];


        gKalmanFilter.P[STATE_Q0][STATE_VX] = gKalmanFilter.P[STATE_VX][STATE_Q0];
        gKalmanFilter.P[STATE_Q3][STATE_VX] = gKalmanFilter.P[STATE_VX][STATE_Q3];
        gKalmanFilter.P[STATE_Q0][STATE_VY] = gKalmanFilter.P[STATE_VY][STATE_Q0];
        gKalmanFilter.P[STATE_Q3][STATE_VY] = gKalmanFilter.P[STATE_VY][STATE_Q3];
#endif

#endif
        return 1;
    }
    else
    {
        return 0;
    }
}