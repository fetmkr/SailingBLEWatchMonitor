#include "heading_filter.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>

constexpr float pi = 3.14159265358979323846f;
float rad(float d) { return d*pi/180; }
float error(float a, float b) { return fabsf(remainderf(a-b,360)); }
void expect(bool good, const char* why) { if (!good) { fprintf(stderr,"FAIL: %s\n",why); exit(1); } }

// Independent ZYX rotation matrix: body -> north/east/down. The reference
// specific force is UP, magnetic field has 30 µT north and 40 µT down.
void reference(float yaw, float pitch, float roll, float a[3], float m[3]) {
    const float cy=cosf(rad(yaw)), sy=sinf(rad(yaw)), cp=cosf(rad(pitch)), sp=sinf(rad(pitch));
    const float cr=cosf(rad(roll)), sr=sinf(rad(roll));
    const float north[3]={cy*cp, cy*sp*sr-sy*cr, cy*sp*cr+sy*sr};
    const float down[3]={-sp, cp*sr, cp*cr};
    for(int i=0;i<3;i++) { a[i]=-down[i]; m[i]=30*north[i]+40*down[i]; }
}
void sensor(const float frd[3], const hdg::HeadingCfg& c, bool mpu, float out[3]) {
    float m[3]; m[c.axisB]=frd[0]*c.signB; m[c.axisA]=-frd[1]*c.signA;
    const int f=c.axisB,r=c.axisA;
    const int permutation=((f==0&&r==1)||(f==1&&r==2)||(f==2&&r==0))?1:-1;
    m[3-f-r]=frd[2]*permutation*c.signB*-c.signA;
    out[0]=mpu?m[1]:m[0];out[1]=mpu?m[0]:m[1];out[2]=mpu?-m[2]:m[2];
}
struct Rig {
    heading::Filter filter;
    hdg::HeadingCfg cfg;
    uint32_t now=10000, tick=10000, revision=1;
    float lastMag[3]={}; uint32_t magAt=0;
    Rig() { cfg.axisA=1; cfg.axisB=2; cfg.signA=cfg.signB=1; }
    heading::Reading step(float yaw, float pitch=0, float roll=0, float rate=0,
                          float ax=0, float ay=0, float magYaw=0, bool magOn=true) {
        now+=10; tick+=10;
        float a[3],m[3],ignored[3],g[3]={-rate*sinf(rad(pitch)), rate*sinf(rad(roll))*cosf(rad(pitch)), rate*cosf(rad(roll))*cosf(rad(pitch))};
        reference(yaw,pitch,roll,a,m); a[0]+=ax; a[1]+=ay;
        if(magOn && (magAt==0 || now/130 != magAt/130)) {
            reference(yaw+magYaw,pitch,roll,ignored,m); sensor(m,cfg,false,lastMag);magAt=now;
        }
        float av[3],gv[3];sensor(a,cfg,true,av);sensor(g,cfg,true,gv);
        filter.update(tick,now,revision,cfg,av,gv,lastMag,magAt?now-magAt:UINT32_MAX);
        return filter.latest(now);
    }
    void settle(float yaw, float pitch=0, float roll=0) { for(int i=0;i<1000;i++) step(yaw,pitch,roll); }
};
int main() {
    int staticCases=0;
    for(int a=0;a<3;a++) for(int b=0;b<3;b++) if(a!=b)
    for(float sa:{-1.f,1.f}) for(float sb:{-1.f,1.f})
    for(float yaw:{0.f,45.f,90.f,135.f,180.f,225.f,270.f,315.f})
    for(float tilt:{-30.f,0.f,30.f}) {
        Rig r;r.cfg.axisA=a;r.cfg.axisB=b;r.cfg.signA=sa;r.cfg.signB=sb;
        r.cfg.offDeg=17;r.cfg.declDeg=-6;r.settle(yaw,tilt/2,tilt);
        expect(error(r.filter.latest(r.now).degrees,hdg::wrap360(yaw+11))<.15,"static heading, axis or offset");staticCases++;
    }
    printf("PASS %d static orientations across 24 axis mappings\n",staticCases);
    float worstTurn=0;
    for(float tilt:{0.f,30.f}) {
        Rig r;r.settle(350,tilt/2,tilt);
        for(int i=1;i<=400;i++) {
            const float truth=hdg::wrap360(350+i*.45f);
            const auto v=r.step(truth,tilt/2,tilt,45);
            worstTurn=std::max(worstTurn,error(v.rawDegrees,truth));
        }
    }
    printf("Undamped Fusion turn 45 deg/s, including north wrap: max error %.3f deg\n",worstTurn);
    expect(worstTurn<3,"real turns must stay responsive, including tilted turn");
    Rig walk;walk.settle(45);float worstWalk=0,rawWorst=0;bool sawAccelReject=false,sawFalseCaution=false;
    for(int i=0;i<1500;i++) {
        const float ax=.5f*sinf(2*pi*2*i*.01f),ay=.35f*sinf(2*pi*3*i*.01f);
        const auto v=walk.step(45,0,0,0,ax,ay);worstWalk=std::max(worstWalk,error(v.degrees,45));
        sawAccelReject|=v.accelIgnored;sawFalseCaution|=v.accelIgnored&&v.caution;
        float a[3],m[3],av[3],mv[3];reference(45,0,0,a,m);a[0]+=ax;a[1]+=ay;
        sensor(a,walk.cfg,true,av);sensor(m,walk.cfg,false,mv);
        rawWorst=std::max(rawWorst,error(hdg::tiltHeadingDeg(av,mv,walk.cfg,false),45));
    }
    printf("Known fixed heading with walking acceleration: fusion %.3f vs instantaneous %.3f deg max error\n",worstWalk,rawWorst);
    expect(worstWalk<5 && rawWorst>20,"reject translation without hiding heading");
    expect(sawAccelReject && !sawFalseCaution,"normal motion rejection must not mark heading uncertain");
    Rig noisy;noisy.settle(45);float rawNoise=0,displayNoise=0;
    for(int i=0;i<1200;i++) {
        const float magneticNoise=((i/13)&1)?8.f:-8.f;
        const auto v=noisy.step(45,0,0,0,0,0,magneticNoise);
        if(i>200) { rawNoise=std::max(rawNoise,error(v.rawDegrees,45));displayNoise=std::max(displayNoise,error(v.degrees,45)); }
    }
    printf("Alternating magnetic heading noise: raw %.3f vs damped %.3f deg max error\n",rawNoise,displayNoise);
    expect(displayNoise<rawNoise*0.75f,"one-second circular damping must reduce visible heading noise");
    Rig disturbance;disturbance.settle(45);float worstMag=0;bool sawReject=false;
    for(int i=0;i<100;i++) {
        auto v=disturbance.step(45,0,0,0,0,0,90);worstMag=std::max(worstMag,error(v.degrees,45));sawReject|=v.magIgnored&&v.caution;
    }
    expect(worstMag<3&&sawReject,"short magnetic disturbance, flagged but visible");
    for(int i=0;i<400;i++) disturbance.step(45);
    expect(error(disturbance.filter.latest(disturbance.now).degrees,45)<1,"recover from temporary magnetic disturbance");
    Rig stale;stale.settle(70);
    for(int i=0;i<60;i++) stale.step(70,0,0,0,0,0,0,false);
    expect(stale.filter.latest(stale.now).degrees>=0&&stale.filter.latest(stale.now).caution,"short mag loss coasts with caution");
    for(int i=0;i<200;i++) stale.step(70,0,0,0,0,0,0,false);
    expect(stale.filter.latest(stale.now).degrees<0,"long mag loss unavailable");
    stale.settle(70);expect(error(stale.filter.latest(stale.now).degrees,70)<1,"mag recovery");
    expect(stale.filter.latest(stale.now+251).degrees<0,"stale IMU cannot look live");
    Rig gaps;gaps.settle(90);gaps.now+=2000;gaps.tick+=2000;
    expect(error(gaps.step(90,0,0,300).degrees,90)<.1,"gap does not integrate final gyro over missing time");
    gaps.revision++;gaps.cfg.offDeg=20;gaps.magAt=0;
    expect(error(gaps.step(180).degrees,200)<.1,"reattach/config reset uses new measurement");
    float a[3]={0,-1,0},g[3]={0,0,300},m[3]={40,-30,0};
    auto before=gaps.filter.latest(gaps.now).degrees;
    gaps.filter.update(gaps.tick,gaps.now,gaps.revision,gaps.cfg,a,g,m,0);
    expect(error(before,gaps.filter.latest(gaps.now).degrees)<.001,"duplicate sample ignored");
    g[1]=std::numeric_limits<float>::quiet_NaN();
    gaps.filter.update(gaps.tick+10,gaps.now+10,gaps.revision,gaps.cfg,a,g,m,0);
    expect(gaps.filter.latest(gaps.now+10).degrees<0,"nonfinite gyro cannot poison state");
    gaps.magAt=0;expect(gaps.step(180).degrees>=0,"invalid input recovery");
    Rig wrap;wrap.now=UINT32_MAX-4000;wrap.tick=wrap.now;wrap.settle(90);
    expect(error(wrap.filter.latest(wrap.now).degrees,90)<.1,"millisecond wrap");
    Rig noMag;float z[3]={};noMag.filter.update(1,1,0,noMag.cfg,a,z,z,0);
    expect(noMag.filter.latest(1).degrees<0,"zero magnetic input cannot create north");
    puts("PASS disturbances, missing/repeated samples, calibration changes, NaN recovery, clock wrap");
}
