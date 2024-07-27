#include <AccelStepper.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#define dirPinRight 12
#define stepPinRight 14
#define dirPinLeft 27
#define stepPinLeft 26
#define servoPin 25

#define motorInterfaceType 1

AccelStepper stepperRight(motorInterfaceType, stepPinRight, dirPinRight);
AccelStepper stepperLeft(motorInterfaceType, stepPinLeft, dirPinLeft);
Servo myservo;

const float wheelDiameter = 22.5;  // in mm
const int stepsPerRevolution = 23; // Assuming each motor has 23 steps per revolution
const float gearReduction = 14.4;
const float effectiveStepsPerRevolution = stepsPerRevolution * gearReduction;
const float stepsPerMili = (effectiveStepsPerRevolution) / (3.14159 * wheelDiameter);
const float Kturn = 1.00;                                    // Empirical constant to adjust turn radius
const float turnRadius = 96.5 * Kturn;                       // in mm adjusted by Kturn
const float turningCircumference = 2 * 3.14159 * turnRadius; // in mm
float RightMotorStepsRemainder = 0.0;
float LeftMotorStepsRemainder = 0.0;
int Rightpresteps = 0;
int Leftpresteps = 0;

struct PathPoint {
  float x;
  float y;
};

const int undraw = 30;
const int draw = 70;

AsyncWebServer server(80);

QueueHandle_t pathQueue;

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<!DOCTYPE HTML>
<html>
<head>
  <title>ESP32 Robot Control</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; display: flex; flex-direction: column; justify-content: center; align-items: center; height: 100vh; margin: 0; }
    canvas { border: 1px solid #000; width: 1000px; height: 1000px; } /* 1m x 1m canvas */
    button { margin-top: 20px; padding: 10px 20px; font-size: 16px; }
  </style>
</head>
<body>
  <canvas id="canvas" width="1000" height="1000"></canvas>
  <button id="sendPathButton">Send Path</button>
  <button id="clearButton">Clear</button>
  <script>
    const canvas = document.getElementById('canvas');
    const ctx = canvas.getContext('2d');
    const sendPathButton = document.getElementById('sendPathButton');
    const clearButton = document.getElementById('clearButton');
    let drawing = false;
    let path = [];

    // Rectangle properties
    const rectWidth = 230;
    const rectHeight = 150;
    const rectX = (canvas.width - rectWidth) / 2; // Centered horizontally
    const rectY = canvas.height - rectHeight; // Bottom edge on bottom line of canvas

    // Mouse events
    canvas.addEventListener('mousedown', (e) => {
      startDrawing(e.clientX, e.clientY);
    });

    canvas.addEventListener('mousemove', (e) => {
      if (drawing) draw(e.clientX, e.clientY, true);
    });

    canvas.addEventListener('mouseup', () => {
      drawing = false;
    });

    // Touch events
    canvas.addEventListener('touchstart', (e) => {
      const touch = e.touches[0];
      startDrawing(touch.clientX, touch.clientY);
    });

    canvas.addEventListener('touchmove', (e) => {
      const touch = e.touches[0];
      if (drawing) draw(touch.clientX, touch.clientY, true);
    });

    canvas.addEventListener('touchend', () => {
      drawing = false;
    });

    sendPathButton.addEventListener('click', () => {
      console.log("Sending path..."); // Log to browser console
      console.log(JSON.stringify({ path })); // Log to browser console
      fetch('/path', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json'
        },
        body: JSON.stringify({ path })
      })
      .then(response => response.text())
      .then(data => console.log(data)); // Log response from server to browser console
    });

    clearButton.addEventListener('click', () => {
      path = [];
      ctx.save();
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);
      ctx.restore();
      drawRectangle(); // Draw the rectangle again after clearing the canvas
      ctx.beginPath();
    });

    function startDrawing(clientX, clientY) {
      drawing = true;
      const x = (clientX - canvas.offsetLeft); // Convert to mm
      const y = (clientY - canvas.offsetTop); // Convert to mm
      ctx.moveTo(x, y);
      path.push({ x: x, y: y, isContinuous: false }); // Start of a new line
    }

    function draw(clientX, clientY, isContinuous) {
      const x = (clientX - canvas.offsetLeft); // Convert to mm
      const y = (clientY - canvas.offsetTop); // Convert to mm
      ctx.lineTo(x, y);
      ctx.stroke();
      path.push({ x: x, y: y, isContinuous: isContinuous });
    }

    function drawRectangle() {
      ctx.fillStyle = 'blue';
      ctx.fillRect(rectX, rectY, rectWidth, rectHeight);
    }

    // Draw the initial rectangle
    drawRectangle();
  </script>
</body>
</html>

)rawliteral";

void setup()
{
  Serial.begin(115200);
  Serial.println("Setup started");

  // Create access point (AP)
  WiFi.softAP("ESP32Robot", "password"); // Change "password" to your desired AP password

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  // Initialize motors and servo
  stepperRight.setMaxSpeed(500);
  stepperRight.setAcceleration(250);
  stepperLeft.setMaxSpeed(500);
  stepperLeft.setAcceleration(250);
  myservo.attach(servoPin);

  // Create the path queue
  pathQueue = xQueueCreate(10, sizeof(DynamicJsonDocument)); //not sure what is queue length

  // Create tasks
  xTaskCreatePinnedToCore(
      serverTask,   /* Task function. */
      "serverTask", /* String with name of task. */
      8192,         /* Stack size in bytes. */
      NULL,         /* Parameter passed as input of the task */
      1,            /* Priority of the task. */
      NULL,         /* Task handle. */
      1);           /* Core where the task should run */

  xTaskCreatePinnedToCore(
      motorTask,   /* Task function. */
      "motorTask", /* String with name of task. */
      8192,        /* Stack size in bytes. */
      NULL,        /* Parameter passed as input of the task */
      2,           /* Priority of the task. */
      NULL,        /* Task handle. */
      0);          /* Core where the task should run */

  Serial.println("Setup complete");
}

void loop() {}

void serverTask(void *parameter)
{
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    request->send_P(200, "text/html", index_html);
  });

  server.on("/path", HTTP_POST, [](AsyncWebServerRequest *request)
  {
    // This handler won't be called until request->send() is called within the body handler
  },
  NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
  {
    if (!index)
    {
      Serial.println("Body start");
      request->_tempObject = malloc(total + 1);
      if (request->_tempObject == nullptr)
      {
        Serial.println("Failed to allocate memory");
        request->send(500, "text/plain", "Server memory allocation failed");
        return;
      }
    }

    memcpy((uint8_t*)request->_tempObject + index, data, len);
    yield();
    delay(1);

    if (index + len == total)
    {
      ((uint8_t*)request->_tempObject)[total] = '\0';
      Serial.println("Body complete");

      String json = String((char*)request->_tempObject);
      free(request->_tempObject);
      request->_tempObject = nullptr;

      Serial.print("Received JSON: ");
      Serial.println(json);

      DynamicJsonDocument *doc = new DynamicJsonDocument(total); // Allocate on heap

      DeserializationError error = deserializeJson(*doc, json);

      if (error)
      {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        delete doc; // Free the allocated memory
        request->send(400, "text/plain", "Invalid JSON");
        return;
      }

      JsonArray path = (*doc)["path"];
      Serial.println("path size");
      Serial.println(path.size());
      if (xQueueSend(pathQueue, &doc, portMAX_DELAY) != pdPASS)
      {
        Serial.println("Failed to send to queue");
        delete doc; // Free the allocated memory
        request->send(500, "text/plain", "Queue send failed");
        return;
      }

      request->send(200, "text/plain", "Path received");
    }
  });

  server.begin();
  Serial.println("Server task running");
  for (;;)
  {
    yield();
    delay(1);
  }
}


void motorTask(void *parameter)
{
  Serial.println("Motor task running");

  for (;;)
  {
    DynamicJsonDocument *doc;
    Serial.println("nonon1"); // debugger
    if (xQueueReceive(pathQueue, &doc, portMAX_DELAY) == pdPASS)
    {
      Serial.println("nonon2"); // debugger
      JsonArray path = (*doc)["path"];
      moveRobotAlongPath(path);
      delete doc; // Free the allocated memory
    }

    yield();
    delay(1);
  }
}


void moveRobotAlongPath(JsonArray path) {
  // Determine the size of the path array
  size_t pathSize = path.size();
  
  // Allocate memory for the C++ array
  PathPoint* pathArray = new PathPoint[pathSize];

  // Convert JsonArray to C++ array
  for (size_t i = 0; i < pathSize; i++) { 
    
    pathArray[i].x = path[i]["x"].as<float>();
    pathArray[i].y = path[i]["y"].as<float>();
    Serial.print(pathArray[i].x);
    Serial.print(", ");
    Serial.println(pathArray[i].y);
    
  }
  
  // Now you can use pathArray instead of path
  int i;
  float XPrev = 0;
  float YPrev = 0;
  float XPrevPrev = 0;
  float YPrevPrev = 0;
  float angleDiff = 0;
  float angle0 = 0;
  float distance = 0;
  angle0 = atan2((pathArray[1].x-pathArray[0].x) , (-pathArray[1].y+pathArray[0].y)); // adding pi to rotate the axes

  for (i = 1; i < pathSize; i=i+8) { // Initialize i inside the loop i=i+smooth factor
    Serial.print("i: ");
    Serial.println(i);
    float x = pathArray[i].x - pathArray[0].x; // centering x
    float y = pathArray[i].y - pathArray[0].y; // centering y
    

    Serial.print("Moving to point: ");
    Serial.print(x);
    Serial.print(", ");
    Serial.println(y);

    distance = sqrt(pow(x - XPrev, 2) + pow(y - YPrev, 2));

    if(i==1)
    {angleDiff = angle0;}
    else{
    angleDiff = atan2(x - XPrev, -y + YPrev) - atan2(XPrev - XPrevPrev, -YPrev + YPrevPrev) ; 
    }
    Serial.print("Y Prev: ");      Serial.print(YPrev); 
          /*Serial.print("Y: ");      Serial.print(y);      Serial.print("Y Prev: ");      Serial.print(YPrev);      Serial.print("YPrevPrev: ");      Serial.println(YPrevPrev);      Serial.print("X: ");      Serial.print(x);      Serial.print("X Prev: ");      Serial.print(XPrev);      Serial.print("XPrevPrev: ");      Serial.println(XPrevPrev); //debugger*/
      
    // Move in arc
    Serial.println("angleDiff");
    Serial.println(angleDiff);
    Serial.println("distance");
    Serial.println(distance);
    moveInArc(distance, angleDiff);
    //at the end of move in arc there shouldn't be an angle diff

    // Add yield to avoid watchdog timer resets
    yield();
    delay(1);

    // Update prev, current positions
    XPrevPrev = XPrev;
    YPrevPrev = YPrev;
    XPrev = x;
    YPrev = y;
    /*Serial.print("Y: ");    Serial.print(y);    Serial.print("Y Prev: ");    Serial.print(YPrev);    Serial.print("YPrevPrev: ");    Serial.println(YPrevPrev);    Serial.print("X: ");    Serial.print(x);    Serial.print("X Prev: ");    Serial.print(XPrev);    Serial.print("XPrevPrev: ");    Serial.println(XPrevPrev); //debugger*/
  }

  // Free the allocated memory
  delete[] pathArray;
}

void moveInArc(float distance, float angleDiff)
{
  // Calculate steps for each motor
  int RightMotorSteps = (int)(stepsPerMili * (distance - turnRadius * angleDiff));
  RightMotorStepsRemainder += (stepsPerMili * (distance - turnRadius * angleDiff)) - RightMotorSteps;

  int LeftMotorSteps = (int)(stepsPerMili * (distance + turnRadius * angleDiff));
  LeftMotorStepsRemainder += (stepsPerMili * (distance + turnRadius * angleDiff)) - LeftMotorSteps;

  // Move both motors
  stepperRight.moveTo(RightMotorSteps + Rightpresteps);
  stepperLeft.moveTo(-LeftMotorSteps - Leftpresteps);
  
  Serial.println("RightMotorSteps"); //debugger
  Serial.println(RightMotorSteps + Rightpresteps);
  Serial.println("LeftMotorSteps");
  Serial.println(-LeftMotorSteps - Leftpresteps);
  
  Rightpresteps = RightMotorSteps + Rightpresteps;
  Leftpresteps = LeftMotorSteps + Leftpresteps;
  
  //Serial.print("Rightpresteps: ");  //Serial.print(Rightpresteps);  //Serial.print(", Leftpresteps: ");  //Serial.println(Leftpresteps);

  while (stepperRight.distanceToGo() != 0 || stepperLeft.distanceToGo() != 0)
  {
    stepperRight.run();
    stepperLeft.run();
    //Serial.println(stepperRight.distanceToGo()); //debugger    //Serial.println(stepperLeft.distanceToGo());    yield();
    delay(1);
  }
}