#include "Arduino.h"
#include "config.h"
#include "def.h"
#include "types.h"
#include "MultiWii.h"
#include "IMU.h"
#include "Sensors.h"



//Specify the links and initial tuning parameters for Alternative PID 
float errSum, lastErr, Output, altVel;
int tempAlt = 0;
unsigned long lastTime = 0;



void getEstimatedAttitude();

void computeIMU () {
  uint8_t axis;
  static int16_t gyroADCprevious[3] = {0,0,0};
  static int16_t gyroADCinter[3];

  uint16_t timeInterleave = 0;
  #if ACC
    ACC_getADC();
    getEstimatedAttitude();
  #endif
  #if GYRO
    Gyro_getADC();
  #endif
  for (axis = 0; axis < 3; axis++)
    gyroADCinter[axis] =  imu.gyroADC[axis];
  timeInterleave=micros();
  annexCode();
  uint8_t t=0;
  while((int16_t)(micros()-timeInterleave)<650) t=1; //empirical, interleaving delay between 2 consecutive reads
  #ifdef LCD_TELEMETRY
    if (!t) annex650_overrun_count++;
  #endif
  #if GYRO
    Gyro_getADC();
  #endif
  for (axis = 0; axis < 3; axis++) {
    gyroADCinter[axis] =  imu.gyroADC[axis]+gyroADCinter[axis];
    // empirical, we take a weighted value of the current and the previous values
    imu.gyroData[axis] = (gyroADCinter[axis]+gyroADCprevious[axis])/3;
    gyroADCprevious[axis] = gyroADCinter[axis]>>1;
    if (!ACC) imu.accADC[axis]=0;
  }
  #if defined(GYRO_SMOOTHING)
    static int16_t gyroSmooth[3] = {0,0,0};
    for (axis = 0; axis < 3; axis++) {
      imu.gyroData[axis] = (int16_t) ( ( (int32_t)((int32_t)gyroSmooth[axis] * (conf.Smoothing[axis]-1) )+imu.gyroData[axis]+1 ) / conf.Smoothing[axis]);
      gyroSmooth[axis] = imu.gyroData[axis];
    }
  #elif defined(TRI)
    static int16_t gyroYawSmooth = 0;
    imu.gyroData[YAW] = (gyroYawSmooth*2+imu.gyroData[YAW])/3;
    gyroYawSmooth = imu.gyroData[YAW];
  #endif
}

// **************************************************
// Simplified IMU based on "Complementary Filter"
// Inspired by http://starlino.com/imu_guide.html
//
// adapted by ziss_dm : http://www.multiwii.com/forum/viewtopic.php?f=8&t=198
//
// The following ideas was used in this project:
// 1) Rotation matrix: http://en.wikipedia.org/wiki/Rotation_matrix
// 2) Small-angle approximation: http://en.wikipedia.org/wiki/Small-angle_approximation
// 3) C. Hastings approximation for atan2()
// 4) Optimization tricks: http://www.hackersdelight.org/
//
// Currently Magnetometer uses separate CF which is used only
// for heading approximation.
//
// **************************************************

//******  advanced users settings *******************
/* Set the Low Pass Filter factor for ACC
   Increasing this value would reduce ACC noise (visible in GUI), but would increase ACC lag time
   Comment this if  you do not want filter at all.
   unit = n power of 2 */
// this one is also used for ALT HOLD calculation, should not be changed
#ifndef ACC_LPF_FACTOR
  #define ACC_LPF_FACTOR 4 // that means a LPF of 16
#endif

/* Set the Gyro Weight for Gyro/Acc complementary filter
   Increasing this value would reduce and delay Acc influence on the output of the filter*/
#ifndef GYR_CMPF_FACTOR
  #define GYR_CMPF_FACTOR 10 //  that means a CMP_FACTOR of 1024 (2^10)
#endif

/* Set the Gyro Weight for Gyro/Magnetometer complementary filter
   Increasing this value would reduce and delay Magnetometer influence on the output of the filter*/
#define GYR_CMPFM_FACTOR 8 // that means a CMP_FACTOR of 256 (2^8)


typedef struct  {
  int32_t X,Y,Z;
} t_int32_t_vector_def;

typedef struct  {
  uint16_t XL; int16_t X;
  uint16_t YL; int16_t Y;
  uint16_t ZL; int16_t Z;
} t_int16_t_vector_def;

// note: we use implicit first 16 MSB bits 32 -> 16 cast. ie V32.X>>16 = V16.X
typedef union {
  int32_t A32[3];
  t_int32_t_vector_def V32;
  int16_t A16[6];
  t_int16_t_vector_def V16;
} t_int32_t_vector;

//return angle , unit: 1/10 degree
int16_t _atan2(int32_t y, int32_t x){
  float z = y;
  int16_t a;
  uint8_t c;
  c = abs(y) < abs(x);
  if ( c ) {z = z / x;} else {z = x / z;}
  a = 2046.43 * (z / (3.5714 +  z * z));
  if ( c ){
   if (x<0) {
     if (y<0) a -= 1800;
     else a += 1800;
   }
  } else {
    a = 900 - a;
    if (y<0) a -= 1800;
  }
  return a;
}

float InvSqrt (float x){ 
  union{  
    int32_t i;  
    float   f; 
  } conv; 
  conv.f = x; 
  conv.i = 0x5f1ffff9 - (conv.i >> 1); 
  return conv.f * (1.68191409f - 0.703952253f * x * conv.f * conv.f);
}

// signed16 * signed16
// 22 cycles
// http://mekonik.wordpress.com/2009/03/18/arduino-avr-gcc-multiplication/
#define MultiS16X16to32(longRes, intIn1, intIn2) \
asm volatile ( \
"clr r26 \n\t" \
"mul %A1, %A2 \n\t" \
"movw %A0, r0 \n\t" \
"muls %B1, %B2 \n\t" \
"movw %C0, r0 \n\t" \
"mulsu %B2, %A1 \n\t" \
"sbc %D0, r26 \n\t" \
"add %B0, r0 \n\t" \
"adc %C0, r1 \n\t" \
"adc %D0, r26 \n\t" \
"mulsu %B1, %A2 \n\t" \
"sbc %D0, r26 \n\t" \
"add %B0, r0 \n\t" \
"adc %C0, r1 \n\t" \
"adc %D0, r26 \n\t" \
"clr r1 \n\t" \
: \
"=&r" (longRes) \
: \
"a" (intIn1), \
"a" (intIn2) \
: \
"r26" \
)

int32_t  __attribute__ ((noinline)) mul(int16_t a, int16_t b) {
  int32_t r;
  MultiS16X16to32(r, a, b);
  //r = (int32_t)a*b; without asm requirement
  return r;
}

// Rotate Estimated vector(s) with small angle approximation, according to the gyro data
void rotateV32( t_int32_t_vector *v,int16_t* delta) {
  int16_t X = v->V16.X;
  int16_t Y = v->V16.Y;
  int16_t Z = v->V16.Z;

  v->V32.Z -=  mul(delta[ROLL]  ,  X)  + mul(delta[PITCH] , Y);
  v->V32.X +=  mul(delta[ROLL]  ,  Z)  - mul(delta[YAW]   , Y);
  v->V32.Y +=  mul(delta[PITCH] ,  Z)  + mul(delta[YAW]   , X);
}

static int16_t accZ=0;

void getEstimatedAttitude(){
  uint8_t axis;
  int32_t accMag = 0;
  float scale;
  int16_t deltaGyroAngle16[3];
  static t_int32_t_vector EstG = {0,0,(int32_t)ACC_1G<<16};
  #if MAG
    static t_int32_t_vector EstM;
  #else
    static t_int32_t_vector EstM = {0,(int32_t)1<<24,0};
  #endif
  static uint32_t LPFAcc[3];
  float invG; // 1/|G|
  static int16_t accZoffset = 0;
  int32_t accZ_tmp=0;
  static uint16_t previousT;
  uint16_t currentT = micros();

  // unit: radian per bit, scaled by 2^16 for further multiplication
  // with a delta time of 3000 us, and GYRO scale of most gyros, scale = a little bit less than 1
  scale = (currentT - previousT) * (GYRO_SCALE * 65536);
  previousT = currentT;

  // Initialization
  for (axis = 0; axis < 3; axis++) {
    // valid as long as LPF_FACTOR is less than 15
    imu.accSmooth[axis]  = LPFAcc[axis]>>ACC_LPF_FACTOR;
    LPFAcc[axis]      += imu.accADC[axis] - imu.accSmooth[axis];
    // used to calculate later the magnitude of acc vector
    accMag   += mul(imu.accSmooth[axis] , imu.accSmooth[axis]);
    // unit: radian scaled by 2^16
    // imu.gyroADC[axis] is 14 bit long, the scale factor ensure deltaGyroAngle16[axis] is still 14 bit long
    deltaGyroAngle16[axis] = imu.gyroADC[axis]  * scale;
  }

  // we rotate the intermediate 32 bit vector with the radian vector (deltaGyroAngle16), scaled by 2^16
  // however, only the first 16 MSB of the 32 bit vector is used to compute the result
  // it is ok to use this approximation as the 16 LSB are used only for the complementary filter part
  rotateV32(&EstG,deltaGyroAngle16);
  rotateV32(&EstM,deltaGyroAngle16);

  // Apply complimentary filter (Gyro drift correction)
  // If accel magnitude >1.15G or <0.85G and ACC vector outside of the limit range => we neutralize the effect of accelerometers in the angle estimation.
  // To do that, we just skip filter, as EstV already rotated by Gyro
  for (axis = 0; axis < 3; axis++) {
    if ( (int16_t)(0.85*ACC_1G*ACC_1G/256) < (int16_t)(accMag>>8) && (int16_t)(accMag>>8) < (int16_t)(1.15*ACC_1G*ACC_1G/256) )
      EstG.A32[axis] += (int32_t)(imu.accSmooth[axis] - EstG.A16[2*axis+1])<<(16-GYR_CMPF_FACTOR);
    accZ_tmp += mul(imu.accSmooth[axis] , EstG.A16[2*axis+1]);
    #if MAG
      EstM.A32[axis]  += (int32_t)(imu.magADC[axis] - EstM.A16[2*axis+1])<<(16-GYR_CMPFM_FACTOR);
    #endif
  }
  
  if (EstG.V16.Z > ACCZ_25deg)
    f.SMALL_ANGLES_25 = 1;
  else
    f.SMALL_ANGLES_25 = 0;

  // Attitude of the estimated vector
  int32_t sqGX_sqGZ = mul(EstG.V16.X,EstG.V16.X) + mul(EstG.V16.Z,EstG.V16.Z);
  invG = InvSqrt(sqGX_sqGZ + mul(EstG.V16.Y,EstG.V16.Y));
  att.angle[ROLL]  = _atan2(EstG.V16.X , EstG.V16.Z);
  att.angle[PITCH] = _atan2(EstG.V16.Y , InvSqrt(sqGX_sqGZ)*sqGX_sqGZ);

  //note on the second term: mathematically there is a risk of overflow (16*16*16=48 bits). assumed to be null with real values
  att.heading = _atan2(
    mul(EstM.V16.Z , EstG.V16.X) - mul(EstM.V16.X , EstG.V16.Z),
    (EstM.V16.Y * sqGX_sqGZ  - (mul(EstM.V16.X , EstG.V16.X) + mul(EstM.V16.Z , EstG.V16.Z)) * EstG.V16.Y)*invG );
  #if MAG
    att.heading += conf.mag_declination; // Set from GUI
  #endif
  att.heading /= 10;

  #if defined(THROTTLE_ANGLE_CORRECTION)
    cosZ = mul(EstG.V16.Z , 100) / ACC_1G ;                                                   // cos(angleZ) * 100 
    throttleAngleCorrection = THROTTLE_ANGLE_CORRECTION * constrain(100 - cosZ, 0, 100) >>3;  // 16 bit ok: 200*150 = 30000  
  #endif

  // projection of ACC vector to global Z, with 1G subtructed
  // Math: accZ = A * G / |G| - 1G
  accZ = accZ_tmp *  invG;
  if (!f.ARMED) {
    accZoffset -= accZoffset>>3;
    accZoffset += accZ;
  }  
  accZ -= accZoffset>>3;
}

#define UPDATE_INTERVAL     25000    // 40hz update rate (20hz LPF on acc)
#define ALT_UPDATE_INTERVAL 5000 //10000 works

#define BARO_TAB_SIZE   21

#define ACC_Z_DEADBAND (ACC_1G>>5) // was 40 instead of 32 now


#define applyDeadband(value, deadband)  \
  if(abs(value) < deadband) {           \
    value = 0;                          \
  } else if(value > 0){                 \
    value -= deadband;                  \
  } else if(value < 0){                 \
    value += deadband;                  \
  }

#if BARO || SONAR || LIDAR
static int32_t  BaroHome = 0;

//Code by CB

long runningAverage(int M)
{
  static int LM[10];      // LastMeasurements
  static byte index = 0;
  static long sum = 0;
  static byte count = 0;

  // keep sum updated to improve speed.
  sum -= LM[index];
  LM[index] = M;
  sum += LM[index];
  index = index % 10;
  if (count < 10) count++;

  return sum / count;
}

int smooth(int data, float filterVal, float smoothedVal){
  if (filterVal > 1){      // check to make sure param's are within range
    filterVal = .99;
  }
  else if (filterVal <= 0){
    filterVal = 0;
  }

  smoothedVal = (data * (1 - filterVal)) + (smoothedVal  *  filterVal);

  return (int)smoothedVal;
}
 
// Edn code CB 
 
uint8_t getEstimatedAltitude(){
	int32_t  BaroAlt;
	static float baroGroundTemperatureScale, logBaroGroundPressureSum;
	static float vel = 0.0f;
	static uint16_t previousT;
	uint16_t currentT = micros();
	uint16_t dTime;
  uint8_t aggressiveness = 1;
  uint8_t readingError = 0;
  float iValue = 0;

	dTime = currentT - previousT;
	if (dTime < ALT_UPDATE_INTERVAL) return 0;
	previousT = currentT;

	if (calibratingB > 0) {
		logBaroGroundPressureSum = log(baroPressureSum);
		baroGroundTemperatureScale = ((int32_t) baroTemperature + 27315) * 29.271267f;// (2 * 29.271267f); // 2 *  is included here => no need for * 2  on BaroAlt in additional LPF
		calibratingB--;
	}

	// baroGroundPressureSum is not supposed to be 0 here
	// see: https://code.google.com/p/ardupilot-mega/source/browse/libraries/AP_Baro/AP_Baro.cpp
	BaroAlt = (logBaroGroundPressureSum - log(baroPressureSum)) * baroGroundTemperatureScale;
  BaroAlt = runningAverage(BaroAlt); //additional filter by C.B.
        
        
	//alt.EstAlt = (alt.EstAlt * 6 + BaroAlt ) >> 3; // additional LPF to reduce baro noise (faster by 30 µs)

#if SONAR
	if (calibratingS > 0) {
	  if (!f.ARMED) { //init offset till motors not armed
	    //alt.EstAlt = alt.EstAlt * SONAR_BARO_LPF_LC + sonarAlt * (1 - SONAR_BARO_LPF_LC); // additional LPF to reduce baro noise (faster by 30 µs)
            BaroHome = (alt.EstAlt * 6 + BaroAlt * 2) >> 3; // play with optimal coef. here
	  }
    calibratingS--;
	}
#endif

/*
 * Decide on which alt is displayed when not in one of the two alt hold modes
 */
#if defined(BARO)
  alt.EstAlt = (alt.EstAlt * 6 + 2 * BaroAlt) >> 3; // additional LPF to reduce baro noise (faster by 30 µs)
#elif defined(SONAR)
  alt.EstAlt = alt.EstAlt * SONAR_BARO_LPF_LC + sonarAlt * (1 - SONAR_BARO_LPF_LC);
#elif defined(LIDAR)
  alt.EstAlt = smooth(lidarAlt, LIDAR_LPF_FACTOR, alt.EstAlt);
#endif

/*
 * Decide which alt to use once alt hold (one of two modes) is engaged
 */
if (f.BARO_MODE && !f.SONAR_MODE && !f.LIDAR_MODE){
   alt.EstAlt = (alt.EstAlt * 6 + 2* BaroAlt) >> 3; // additional LPF to reduce baro noise (faster by 30 µs)
}
       
else if (f.SONAR_MODE && !f.BARO_MODE && !f.LIDAR_MODE) {
    alt.EstAlt = sonarAlt;
}

else if (f.LIDAR_MODE && !f.BARO_MODE && !f.SONAR_MODE) {//no Sonar if LIDAR is present
    alt.EstAlt = smooth(lidarAlt, LIDAR_LPF_FACTOR, alt.EstAlt);  
}

else if (f.LIDAR_MODE && f.SONAR_MODE && !f.BARO_MODE){
  //use sonar values to check lidar or baro readings for correctness
  readingError = abs(lidarAlt-sonarAlt);
  if ((readingError > 5) && (sonarAlt <= SONAR_MAX_HOLD)) //if there is a difference and we are below sonar max range
  {
    alt.EstAlt = min(sonarAlt,lidarAlt); //just for safety we are using the lower reading
  }
  else
  {
    //Both readings are in the same range or alt is beyond sonar range. Thus, we depend on the Lidar since it
    //the Lidar has a higher range (~40m)
   alt.EstAlt = lidarAlt;
  }
}
else if  ((f.SONAR_MODE || f.LIDAR_MODE) && f.BARO_MODE) {
    //This mode will allow for holding absolute altitude and verifies readings by using
    //either Sonar or Lidar.
    //Alternatively we can do a sensor fusion. However, this would mean that we mix
    //relative and absolute altitude hold.
}   

//
//          	if (sonarAlt < SONAR_BARO_FUSION_LC) {
//  		  alt.EstAlt = alt.EstAlt * SONAR_BARO_LPF_LC + (BaroHome + sonarAlt) * (1 - SONAR_BARO_LPF_LC); // additional LPF to reduce baro noise (faster by 30 µs)
//                  //alt.EstAlt = sonarAlt;
//                  //alt.EstAlt = (alt.EstAlt * 6 + sonarAlt * 2) >> 3;
//	        }
//	        else if (sonarAlt < SONAR_BARO_FUSION_HC) {
//    		  float fade = SONAR_BARO_FUSION_RATIO;
//    		  if (fade == 0.0) fade = ((float) (SONAR_BARO_FUSION_HC - sonarAlt)) / (SONAR_BARO_FUSION_HC - SONAR_BARO_FUSION_LC);
//    		  fade = constrain(fade, 0.0f, 1.0f);
//    
//    		  // LOG: will LPF should be faded too ? sonar is less sloppy than baro and will be oversmoothed
//    		  // LOG: try same as baro alone 6/4 ratio (same as above about smoothing)
//    		  alt.EstAlt = alt.EstAlt * SONAR_BARO_LPF_HC + ((BaroHome + sonarAlt) * fade + (BaroAlt) * (1 - fade)) * (1 - SONAR_BARO_LPF_HC);
//	        }
//                else {
//                alt.EstAlt = (alt.EstAlt * 6 + BaroAlt) >> 3; // additional LPF to reduce baro noise (faster by 30 µs)
//        	}
//        }

	//alt.EstAlt = alt.EstAlt * SONAR_BARO_LPF_LC + sonarAlt * (1 - SONAR_BARO_LPF_LC); // SONAR
//#endif

#if (defined(VARIOMETER) && (VARIOMETER != 2)) || !defined(SUPPRESS_BARO_ALTHOLD)


        if (f.SONAR_MODE || f.LIDAR_MODE) //Relative_Alt Mode
        {
          // Time difference to last call
          unsigned long now = millis();
          unsigned long timeChange = (now - lastTime);
  
          //Filter out strange  readings. If there is a gap of
          //2m between two readings (~160ms) there was a problem. 
          if ((abs(alt.EstAlt - tempAlt) > 200) && (tempAlt != 0) && (timeChange < 2000)) {
            alt.EstAlt = tempAlt;
          }
          else {
            tempAlt = alt.EstAlt;
            //Use adaptive tunings. If we are more than 4m away from our target we will use more aggressive PIDs
            if  (abs(AltHold - alt.EstAlt) > 400) {
                aggressiveness = 1.2; 
            }
            else {
                aggressiveness = 1;
            }
          }
                    
          //Compute all the working error variables
          int16_t error16 = constrain(AltHold - alt.EstAlt, -300, 300);
          applyDeadband(error16, 5); //remove small P parameter to reduce noise near zero position //was 10
          
          float dErr = (error16 - lastErr) / timeChange; 

          errSum += (error16 * timeChange);
          errSum = constrain(errSum, -1000000, 1000000); 

          //Compute PID Output
              
          float pValue = (conf.pid[PIDALT].P8 * error16  * aggressiveness);   

          // V velocity, cm/sec
           altVel += accZ * ACC_VelScale * dTime;
          //avoid integration when setting AltHold or when the copter rises/falls
          if (altVel < 5)
          {
            errSum = constrain(errSum, -20, 20); //
            iValue = conf.pid[PIDALT].I8 * errSum;
          } 
    
          float dValue = conf.pid[PIDALT].D8 * dErr;
              
          Output =  (pValue / 100 )+ (iValue / 100000)  - (dValue / 100);  //The test quad is ligth and powerfull. Thus we divide P by 100 to reduce the strength of reactions
          Output = constrain(Output, -150, 150); //to avoid reactions that are too strong

          if (error16 < 0)
          {
            //we want to lower the copters' altitude. This requires more effort since we do only have upward thrust
            //Thus we increase our PID value to 105%
            Output = Output * 1.01;
          }
          //Remember some variables for the next loop
          lastErr = error16; //pValue;
          lastTime = now;      
          //Hand over new value
          BaroPID = (int) Output;    
                      
   //       debug[0] = BaroPID; 
   //       debug[1] = alt.EstAlt;
   //       debug[2] = AltHold;//lidarAlt;
          
        }
        else { //Absolute_Alt Mode
	          //P
	          int16_t error16 = constrain(AltHold - alt.EstAlt, -300, 300);
      	    applyDeadband(error16, 10); //remove small P parametr to reduce noise near zero position
      	    BaroPID = constrain((conf.pid[PIDALT].P8 * error16 >> 7), -150, +150);
            //I
      	    errorAltitudeI += conf.pid[PIDALT].I8 * error16 >> 6;
      	    errorAltitudeI = constrain(errorAltitudeI, -30000, 30000);
      	    BaroPID += errorAltitudeI >> 9; //I in range +/-60
      
      	    applyDeadband(accZ, ACC_Z_DEADBAND);
      
      	    static int32_t lastBaroAlt;
      	    // could only overflow with a difference of 320m, which is highly improbable here
      	    int16_t baroVel = mul((alt.EstAlt - lastBaroAlt), (1000000 / UPDATE_INTERVAL));
      
      	    lastBaroAlt = alt.EstAlt;
      
      	    baroVel = constrain(baroVel, -300, 300); // constrain baro velocity +/- 300cm/s
      	    applyDeadband(baroVel, 10); // to reduce noise near zero
      
      	    // Integrator - velocity, cm/sec
      	    vel += accZ * ACC_VelScale * dTime;
      
      	    // apply Complimentary Filter to keep the calculated velocity based on baro velocity (i.e. near real velocity). 
      	    // By using CF it's possible to correct the drift of integrated accZ (velocity) without loosing the phase, i.e without delay
      	    vel = vel * 0.985f + baroVel * 0.015f;
      
      	    //D
      	    alt.vario = vel;
      	    applyDeadband(alt.vario, 5);	
        }
#endif
	return 1;
}
#endif //BARO


void setSonarHold(int alt){
  
  if (alt < 0) //as a safety feature when performing smooth alt hold changes)
    return;
         
  tempAlt = alt;
  BaroPID = 0;
  altVel = 0;
}

void stopSonarPID() {
  
    AltHold = 0;
    tempAlt = 0;
    BaroPID = 0;
    lastErr = 0;
    lastTime = 0;
    errSum = 0;
}
