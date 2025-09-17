#define PIN_ZX_WR   13
#define PIN_ZX_CAS  12
#define PIN_ZX_RAS  11

#define PIN_ZX_A0   10
#define PIN_ZX_A1    9
#define PIN_ZX_A2    8

#define PIN_ZX_D0    7
#define PIN_ZX_D1    6
#define PIN_ZX_D2    5

void setup()
{
  Serial.begin(9600);

  pinMode(PIN_ZX_RAS, OUTPUT);   digitalWrite(PIN_ZX_RAS, HIGH);
  pinMode(PIN_ZX_CAS, OUTPUT);   digitalWrite(PIN_ZX_CAS, HIGH);
  pinMode(PIN_ZX_WR,  OUTPUT);   digitalWrite(PIN_ZX_WR,  HIGH);

  pinMode(PIN_ZX_A0,  OUTPUT);   digitalWrite(PIN_ZX_A0,  LOW);
  pinMode(PIN_ZX_A1,  OUTPUT);   digitalWrite(PIN_ZX_A1 , LOW);
  pinMode(PIN_ZX_A2,  OUTPUT);   digitalWrite(PIN_ZX_A2,  LOW);

  pinMode(PIN_ZX_D0,  OUTPUT);   digitalWrite(PIN_ZX_D0,  LOW);
  pinMode(PIN_ZX_D1,  OUTPUT);   digitalWrite(PIN_ZX_D1,  LOW);
  pinMode(PIN_ZX_D2,  OUTPUT);   digitalWrite(PIN_ZX_D2,  LOW);

  Serial.println("Initialised\n");
}

typedef struct _transition
{
  unsigned int pin;
  unsigned int high_or_low;
  unsigned int delay;
} TRANSITION;

TRANSITION transitions[] = {

  /* RAS/CAS sequence from 0001.png */
  { PIN_ZX_RAS, LOW, 5 },
  { PIN_ZX_CAS, LOW, 10 },

  { PIN_ZX_CAS, HIGH, 4 },
  { PIN_ZX_CAS, LOW,  5 },

  { PIN_ZX_RAS, HIGH, 8 },
  { PIN_ZX_RAS, LOW,  0 },

  { PIN_ZX_CAS, HIGH, 4 },
  { PIN_ZX_CAS, LOW,  10 },

  { PIN_ZX_CAS, HIGH, 4 },
  { PIN_ZX_CAS, LOW,  4 },

  { PIN_ZX_RAS, HIGH, 8 },
  { PIN_ZX_CAS, HIGH, 100 },

  /* A refresh */
  { PIN_ZX_RAS, LOW,  15 },
  { PIN_ZX_RAS, HIGH, 10 },

  /* RAS/CAS sequence from 0003.png */
  { PIN_ZX_RAS, LOW, 5 },
  { PIN_ZX_CAS, LOW, 20 },

  { PIN_ZX_RAS, HIGH, 4 },
  { PIN_ZX_CAS, HIGH, 10 },

  { PIN_ZX_RAS, LOW, 14 },
  { PIN_ZX_WR,  LOW,  4 },
  { PIN_ZX_CAS, LOW, 10 },

  { PIN_ZX_RAS, HIGH,  0 },
  { PIN_ZX_WR, HIGH,  4 },

  { PIN_ZX_CAS, HIGH, 0 },

  { 0, 0, 0 }
};

void loop()
{
  Serial.print("START\n");

  unsigned int i=0;
  while( transitions[i].pin != 0 )
  {
    digitalWrite( transitions[i].pin, transitions[i].high_or_low );

    delay( transitions[i].delay );

    //delayMicroseconds( transitions[i].delay );

    //unsigned int loop;
    //for( loop=0; loop < transitions[i].delay; loop++ );

    i++;
  }

  Serial.print("END\n");

  delay(5000);

  digitalWrite(PIN_ZX_RAS, HIGH);
  digitalWrite(PIN_ZX_CAS, HIGH);
  digitalWrite(PIN_ZX_WR,  HIGH);

  digitalWrite(PIN_ZX_A0,  LOW);
  digitalWrite(PIN_ZX_A1 , LOW);
  digitalWrite(PIN_ZX_A2,  LOW);

  digitalWrite(PIN_ZX_D0,  LOW);
  digitalWrite(PIN_ZX_D1,  LOW);
  digitalWrite(PIN_ZX_D2,  LOW);
}



//    delayMicroseconds( transitions[i].pin );
