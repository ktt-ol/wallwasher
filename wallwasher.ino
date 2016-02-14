#include <Arduino.h>
#include <math.h>
#include <ESPDMX.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>

#include "index_html.h"
#include "settings.h"


#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))


#define DEG_TO_RAD(X) (M_PI*(X)/180)

// http://blog.saikoled.com/post/44677718712/how-to-convert-from-hsi-to-rgb-white
void hsi2rgbw(float H, float S, float I, int* rgbw) {
  int r, g, b, w;
  float cos_h, cos_1047_h;

  while (H > 360) {
    H -= 360;
  }
  // H = fmod(H,360); // cycle H around to 0-360 degrees
  H = 3.14159*H/(float)180; // Convert to radians.
  S = S>0?(S<1?S:1):0; // clamp S and I to interval [0,1]
  I = I>0?(I<1?I:1):0;

  if(H < 2.09439) {
    cos_h = cos(H);
    cos_1047_h = cos(1.047196667-H);
    r = S*255*I/3*(1+cos_h/cos_1047_h);
    g = S*255*I/3*(1+(1-cos_h/cos_1047_h));
    b = 0;
    w = 255*(1-S)*I;
  } else if(H < 4.188787) {
    H = H - 2.09439;
    cos_h = cos(H);
    cos_1047_h = cos(1.047196667-H);
    g = S*255*I/3*(1+cos_h/cos_1047_h);
    b = S*255*I/3*(1+(1-cos_h/cos_1047_h));
    r = 0;
    w = 255*(1-S)*I;
  } else {
    H = H - 4.188787;
    cos_h = cos(H);
    cos_1047_h = cos(1.047196667-H);
    b = S*255*I/3*(1+cos_h/cos_1047_h);
    r = S*255*I/3*(1+(1-cos_h/cos_1047_h));
    g = 0;
    w = 255*(1-S)*I;
  }

  rgbw[0]=r;
  rgbw[1]=g;
  rgbw[2]=b;
  rgbw[3]=w;
}

class Color {
public:
    uint8_t r, g, b, a;
    Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) :
        r(r), g(g), b(b), a(a)
    {}
    Color fade(Color c, float f) {
        return Color(
            (uint8_t)(r + (float)(c.r-r)*f),
            (uint8_t)(g + (float)(c.g-g)*f),
            (uint8_t)(b + (float)(c.b-b)*f),
            (uint8_t)(a + (float)(c.a-a)*f)
        );
    }
};

class Washer {
protected:
    int chan;
    float scale;
    DMXESPSerial dmx;
public:
    Washer(DMXESPSerial dmx, int chan) :
        scale(1.0),
        dmx(dmx),
        chan(chan)
    {}

    void setScale(float s) {
        if (s > 1.0) {
            scale = 1.0;
        } else if (s < 0.0) {
            scale = 0;
        } else {
            scale = s;
        }
    }

    virtual void set(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        dmx.write(chan+0, (uint8_t)(float(r)*scale));
        dmx.write(chan+1, (uint8_t)(float(g)*scale));
        dmx.write(chan+2, (uint8_t)(float(b)*scale));
        dmx.write(chan+3, (uint8_t)(float(a)*scale));
    }

    virtual void set(Color c) {
        dmx.write(chan+0, (uint8_t)(float(c.r)*scale));
        dmx.write(chan+1, (uint8_t)(float(c.g)*scale));
        dmx.write(chan+2, (uint8_t)(float(c.b)*scale));
        dmx.write(chan+3, (uint8_t)(float(c.a)*scale));
    }
};


// Spot is a Washer without amber
class Spot : public Washer {
public:
    Spot(DMXESPSerial dmx, int chan) :
        Washer(dmx, chan)
    {}

    virtual void set(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        dmx.write(chan+0, (uint8_t)(float(r)*scale));
        dmx.write(chan+1, (uint8_t)(float(g)*scale));
        dmx.write(chan+2, (uint8_t)(float(b)*scale));
    }

    virtual void set(Color c) {
        dmx.write(chan+0, (uint8_t)(float(c.r)*scale));
        dmx.write(chan+1, (uint8_t)(float(c.g)*scale));
        dmx.write(chan+2, (uint8_t)(float(c.b)*scale));
    }

};

class Wheel {
private:
    bool running;
    unsigned long startTime;
    int duration;
    int intensity;
    int saturation;
    int hueOffset;
public:
    Washer w;

    Wheel(Washer w, int duration, int saturation=70, int intensity=70) :
        w(w), duration(duration),
        saturation(saturation),
        intensity(intensity)
    {}

    void start() {
        running = true;
        startTime = millis();
    }

    void reset() {
        startTime = millis();
    }

    void setHueOffset(int v) {
        hueOffset = v;
    }

    void setDuration(int d) {
        duration = max(500, d);
    }

    void setSaturation(int s) {
        saturation = min(max(0, s), 100);
    }

    void setIntensity(int i) {
        intensity = min(max(0, i), 100);
    }

    void step() {
        unsigned long now = millis();
        unsigned long elapsed = now - startTime;
        float progress = elapsed / (float)duration;
        if (progress > 1.0) {
            progress = 0.0;
            startTime = now;
        }

        int rgbw[4];
        hsi2rgbw(progress*360+hueOffset, (float)saturation/100, (float)intensity/100, &rgbw[0]);

        w.set(rgbw[0], rgbw[1], rgbw[2], rgbw[3]);
    }
};

const int ALL_GROUPS = -1;
const int MAX_WASHERS = 8;

class Wheels {
private:
    Wheel *wheels[MAX_WASHERS];
    int groupIds[MAX_WASHERS];
    int num;
    bool light;
    Color color;
public:
    Wheels() : num(0), light(false),
    color(Color(255, 255, 255, 255))
    {
        for (int i = 0; i < MAX_WASHERS; i++) {
            groupIds[i] = -1;
        }
    }
    void addWheel(int group, Wheel *w) {
        if (num >= MAX_WASHERS) {
            return; // discard
        }
        wheels[num] = w;
        groupIds[num] = group;
        num += 1;
    }

    void start() {
        for (int i = 0; i < num; i++) {
            wheels[i]->start();
        }
    }

    void setWave(int offset, int duration) {
        for (int i = 0; i < num; i++) {
                wheels[i]->setHueOffset(offset*i);
                wheels[i]->setDuration(duration);
                wheels[i]->reset();
        }
    }

    void setDuration(int group, int v) {
        for (int i = 0; i < num; i++) {
            if (group == ALL_GROUPS || groupIds[i] == group) {
                wheels[i]->setDuration(v);
            }
        }
    }

    void setSaturation(int group, int v) {
        for (int i = 0; i < num; i++) {
            if (group == ALL_GROUPS || groupIds[i] == group) {
                wheels[i]->setSaturation(v);
            }
        }
    }

    void setIntensity(int group, int v) {
        for (int i = 0; i < num; i++) {
            if (group == ALL_GROUPS || groupIds[i] == group) {
                wheels[i]->setIntensity(v);
            }
        }
    }

    void toggleLight() {
        light = !light;
    }

    void toggleLight(bool status) {
        light = status;
    }

    void setLightColor(Color c) {
        // clip max power
        if ((c.r + c.g + c.b + c.a) > 800) {
            c.r = (float)(c.r)*0.8;
            c.g = (float)(c.g)*0.8;
            c.b = (float)(c.b)*0.8;
            c.a = (float)(c.a)*0.8;
        }
        color = c;
    }

    void step() {
        for (int i = 0; i < num; i++) {
            if (light) {
                wheels[i]->w.set(color);
            } else {
                wheels[i]->step();
            }
        }
    }


};

DMXESPSerial dmx;

Washer w1(dmx, 1);
Washer w6(dmx, 6);
Washer w11(dmx, 11);
Washer w16(dmx, 16);
Washer w21(dmx, 21);
Washer w26(dmx, 26);
Spot s30(dmx, 30);

Wheel  wh1(w1,  20000);
Wheel  wh6(w6,  60000, 60, 100);
Wheel wh11(w11, 60000, 70, 100);
Wheel wh16(w16, 60000, 60, 100);
Wheel wh21(w21, 25000);
Wheel wh26(w26, 25000);
Wheel wh30(s30, 25000);

Wheels wheels;

ESP8266WebServer server = ESP8266WebServer(80);


void setup() {
    dmx.init(64); // initialize with bus length
    Serial.begin(115200);
    Serial.println();
    Serial.println("Startup!");
    Serial.println();

    WiFi.hostname(host);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    // wait for connection to be established
    while(WiFi.waitForConnectResult() != WL_CONNECTED){
      WiFi.begin(ssid, password);
      Serial.println("WiFi connection failed, retrying.");
      delay(500);
    }

    if(MDNS.begin(host)) {
        Serial.println("MDNS responder started");
    }

    // handle index
    server.on("/", []() {
        server.send(200, "text/html", FPSTR(INDEX_HTML));
    });

    server.on("/light", []() {
        uint8_t r=0, g=0, b=0, a=0;
        for (uint8_t i=0; i<server.args(); i++){
            if (server.argName(i).equals("r")) {
                r = server.arg(i).toInt();
            }
            if (server.argName(i).equals("g")) {
                g = server.arg(i).toInt();
            }
            if (server.argName(i).equals("b")) {
                b = server.arg(i).toInt();
            }
            if (server.argName(i).equals("a")) {
                a = server.arg(i).toInt();
            }
        }
        if (server.args() == 0) {
            wheels.toggleLight();
        } else {
            wheels.setLightColor(Color(r, g, b, a));
            wheels.toggleLight(true);
        }
        server.send(200, "text/plain", "OK");
    });

    server.on("/w", []() {
        int group = -1;
        int duration = -1;
        int intensity = -1;
        int saturation = -1;

        for (uint8_t i=0; i<server.args(); i++){
            if (server.argName(i).equals("d")) {
                duration = server.arg(i).toInt();
            }
            if (server.argName(i).equals("i")) {
                intensity = server.arg(i).toInt();
            }
            if (server.argName(i).equals("s")) {
                saturation = server.arg(i).toInt();
            }
            if (server.argName(i).equals("g")) {
                group = server.arg(i).toInt();
            }
        }
        if (group != -1) {
            if (duration > -1) { wheels.setDuration(group, duration); }
            if (intensity > -1) { wheels.setIntensity(group, intensity); }
            if (saturation > -1) { wheels.setSaturation(group, saturation); }
        }
        Serial.print(group); Serial.print(" ");
        Serial.print(duration); Serial.print(" ");
        Serial.print(intensity); Serial.print(" ");
        Serial.print(saturation); Serial.print(" ");
        Serial.println("");
        server.send(200, "text/plain", "OK");
    });

    server.on("/wave", []() {
        int duration = -1;
        int offset = -1;
        int intensity = -1;
        int saturation = -1;
        for (uint8_t i=0; i<server.args(); i++){
            if (server.argName(i).equals("d")) {
                duration = server.arg(i).toInt();
            }
            if (server.argName(i).equals("o")) {
                offset = server.arg(i).toInt();
            }
            if (server.argName(i).equals("i")) {
                intensity = server.arg(i).toInt();
            }
            if (server.argName(i).equals("s")) {
                saturation = server.arg(i).toInt();
            }
        }
        if (duration != -1 && offset != -1) {
            wheels.setWave(offset, duration);
        }
        if (intensity != -1) {
            wheels.setIntensity(ALL_GROUPS, intensity);
        }
        if (saturation != -1) {
            wheels.setSaturation(ALL_GROUPS, saturation);
        }

        Serial.print(duration); Serial.print(" ");
        Serial.print(offset); Serial.print(" ");
        Serial.println("");
        server.send(200, "text/plain", "OK");
    });

    server.begin();
    MDNS.addService("http", "tcp", 80);

    ArduinoOTA.setHostname(host);
    ArduinoOTA.setPassword(atopassword);
    ArduinoOTA.begin();


    wheels.addWheel(0, &wh1);
    wheels.addWheel(1, &wh6);
    wheels.addWheel(1, &wh11);
    wheels.addWheel(1, &wh16);
    wheels.addWheel(2, &wh21);
    wheels.addWheel(2, &wh26);
    wheels.addWheel(2, &wh30);


    wheels.setWave(30, 20000);
    wheels.setIntensity(ALL_GROUPS, 100);
    wheels.setSaturation(ALL_GROUPS, 60);
    wheels.start();
}


void loop() {
    ArduinoOTA.handle();
    server.handleClient();

    wheels.step();
    dmx.update();
    delay(50);
}
