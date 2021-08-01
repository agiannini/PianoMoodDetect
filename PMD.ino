/* Edge Impulse Arduino examples - Modified by Isaac Song (iiisong)
 * Copyright (c) 2021 EdgeImpulse Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// If your target is limited in memory remove this macro to save 10K RAM
#define EIDSP_QUANTIZE_FILTERBANK   0

/**
 * Define the number of slices per model window. E.g. a model window of 1000 ms
 * with slices per model window set to 4. Results in a slice size of 250 ms.
 * For more info: https://docs.edgeimpulse.com/docs/continuous-audio-sampling
 */
#define EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW 3

/* Includes ---------------------------------------------------------------- */
#include <PDM.h>
#include <pianoMD-nano_inferencing.h>
#include <ArduinoBLE.h>


/* Averaging */
int sampleTime = 10000; // length of each sample in ms
int angrySum = 0; // times of angry over sampleTime
int happySum = 0; // times of happy over sampleTime
int sadSum = 0; // times of sad over sampleTime
int silenceSum = 0; // times of silence over sampleTime

long prevSampleTime = millis(); // timestamp of last sample

/* Bluetooth */
BLEService PMDService("19B10000-E8F2-537E-4F6C-D104768A1214"); // PMD Service
// BLE Mood Characteristic
BLEUnsignedCharCharacteristic moodCharacteristic("6be2e88a-f277-11eb-9a03-0242ac130003",  // standard 16-bit characteristic UUID
    BLERead | BLENotify); 
    /* 
    moodCharacteristic key
    (0x)00 : None (default value or Error)
    (0x)01 : Angry
    (0x)02 : Happy
    (0x)03 : Sad
    (0x)04 : Silence
    */
  
/** Audio buffers, pointers and selectors */
typedef struct {
    signed short *buffers[2];
    unsigned char buf_select;
    unsigned char buf_ready;
    unsigned int buf_count;
    unsigned int n_samples;
} inference_t;

static inference_t inference;
static bool record_ready = false;
static signed short *sampleBuffer;
static bool debug_nn = false; // Set this to true to see e.g. features generated from the raw signal
static int print_results = -(EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW);

/**
 * @brief      Arduino setup function
 */
void setup()
{
    // put your setup code here, to run once:
    Serial.begin(115200);
    
    /* Bluetooth */
    pinMode(LED_BUILTIN, OUTPUT); // initialize the built-in LED pin to indicate when a central is connected

    // begin BLE initialization
    if (!BLE.begin()) {
      Serial.println("starting BLE failed!");

      while (1);
    }

    BLE.setLocalName("PMDMonitor");
    BLE.setAdvertisedService(PMDService); // add the service UUID
    PMDService.addCharacteristic(moodCharacteristic); // add the mood characteristic
    BLE.addService(PMDService); // Add the PMD service
    moodCharacteristic.writeValue(0); // set initial value for this characteristic

    /* Start advertising BLE.  It will start continuously transmitting BLE
     advertising packets and will be visible to remote BLE central devices
     until it receives a new connection */

    // start advertising
    BLE.advertise();

    Serial.println("Edge Impulse Inferencing Demo");

    // summary of inferencing settings (from model_metadata.h)
    ei_printf("Inferencing settings:\n");
    ei_printf("\tInterval: %.2f ms.\n", (float)EI_CLASSIFIER_INTERVAL_MS);
    ei_printf("\tFrame size: %d\n", EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
    ei_printf("\tSample length: %d ms.\n", EI_CLASSIFIER_RAW_SAMPLE_COUNT / 16);
    ei_printf("\tNo. of classes: %d\n", sizeof(ei_classifier_inferencing_categories) /
                                            sizeof(ei_classifier_inferencing_categories[0]));

    run_classifier_init();
    if (microphone_inference_start(EI_CLASSIFIER_SLICE_SIZE) == false) {
        ei_printf("ERR: Failed to setup audio sampling\r\n");
        return;
    }
}

/**
 * @brief      Arduino main function. Runs the inferencing loop.
 */
void loop()
{
    // wait for a BLE central
    BLEDevice central = BLE.central();

    // if a central is connected to the peripheral:
    if (central) {
        Serial.print("Connected to central: ");
        // print the central's BT address:
        Serial.println(central.address());
        // turn on the LED to indicate the connection:
        digitalWrite(LED_BUILTIN, HIGH);

        while (central.connected()) {
            bool m = microphone_inference_record();
            if (!m) {
                ei_printf("ERR: Failed to record audio...\n");
                return;
            }

            signal_t signal;
            signal.total_length = EI_CLASSIFIER_SLICE_SIZE;
            signal.get_data = &microphone_audio_signal_get_data;
            ei_impulse_result_t result = {0};

            EI_IMPULSE_ERROR r = run_classifier_continuous(&signal, &result, debug_nn);
            if (r != EI_IMPULSE_OK) {
                ei_printf("ERR: Failed to run classifier (%d)\n", r);
                return;
            }

            if (++print_results >= (EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW)) {
                // print the predictions
                ei_printf("Predictions ");
                ei_printf("(DSP: %d ms., Classification: %d ms., Anomaly: %d ms.)",
                    result.timing.dsp, result.timing.classification, result.timing.anomaly);
                ei_printf(": \n");

                // temp store % for each mood
                float angryTemp = result.classification[0].value;
                float happyTemp = result.classification[1].value;
                float sadTemp = result.classification[2].value;
                float silenceTemp = result.classification[3].value;

                for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
                    ei_printf("    %s: %.5f\n", result.classification[ix].label,
                              result.classification[ix].value);
                }

                if (max(max(angryTemp, happyTemp), max(sadTemp, silenceTemp)) == angryTemp) {angrySum++;}
                else if (max(max(angryTemp, happyTemp), max(sadTemp, silenceTemp)) == happyTemp) {happySum++;}
                else if (max(max(angryTemp, happyTemp), max(sadTemp, silenceTemp)) == sadTemp) {sadSum++;}
                else if (max(max(angryTemp, happyTemp), max(sadTemp, silenceTemp)) == silenceTemp) {silenceSum++;}


                // every approximate sampleTime interval return most common mood of time
                if (millis() - prevSampleTime >= sampleTime) {
                  Serial.println();
                  Serial.println("Angry: " + String(angrySum));
                  Serial.println("Happy: " + String(happySum));
                  Serial.println("Sad: " + String(sadSum));
                  Serial.println("Silence: " + String(silenceSum));

                  int maxMood = max(max(silenceSum, angrySum), max(happySum, sadSum));
                  Serial.println(maxMood);

                  // if most common mood is angry
                  if (maxMood == angrySum) {
                    moodCharacteristic.writeValue(1);
                    Serial.println("---ANGRY---");
                  }

                  // if most common mood is happy
                  else if (maxMood == happySum) {
                    moodCharacteristic.writeValue(2);
                    Serial.println("---HAPPY---");
                  }

                  // if most common mood is sad
                  else if (maxMood == sadSum) {
                    moodCharacteristic.writeValue(3);
                    Serial.println("---SAD---");
                  }

                  // if most common mood is silence
                  else if (maxMood == silenceSum) {
                    moodCharacteristic.writeValue(4);
                    Serial.println("---SILENCE---");
                  }

                  // reset the variables
                  angrySum = 0;
                  happySum = 0;
                  sadSum = 0;
                  silenceSum = 0;

                  // update previousSampleTime
                  prevSampleTime = millis();
                }

    #if EI_CLASSIFIER_HAS_ANOMALY == 1
            ei_printf("    anomaly score: %.3f\n", result.anomaly);
    #endif

                print_results = 0;
            }
        }

        // when the central disconnects, turn off the LED:
        digitalWrite(LED_BUILTIN, LOW);
        Serial.print("Disconnected from central: ");
        Serial.println(central.address());
    }
}

/**
 * @brief      Printf function uses vsnprintf and output using Arduino Serial
 *
 * @param[in]  format     Variable argument list
 */
void ei_printf(const char *format, ...) {
    static char print_buf[1024] = { 0 };

    va_list args;
    va_start(args, format);
    int r = vsnprintf(print_buf, sizeof(print_buf), format, args);
    va_end(args);

    if (r > 0) {
        Serial.write(print_buf);
    }
}

/**
 * @brief      PDM buffer full callback
 *             Get data and call audio thread callback
 */
static void pdm_data_ready_inference_callback(void)
{
    int bytesAvailable = PDM.available();

    // read into the sample buffer
    int bytesRead = PDM.read((char *)&sampleBuffer[0], bytesAvailable);

    if (record_ready == true) {
        for (int i = 0; i<bytesRead>> 1; i++) {
            inference.buffers[inference.buf_select][inference.buf_count++] = sampleBuffer[i];

            if (inference.buf_count >= inference.n_samples) {
                inference.buf_select ^= 1;
                inference.buf_count = 0;
                inference.buf_ready = 1;
            }
        }
    }
}

/**
 * @brief      Init inferencing struct and setup/start PDM
 *
 * @param[in]  n_samples  The n samples
 *
 * @return     { description_of_the_return_value }
 */
static bool microphone_inference_start(uint32_t n_samples)
{
    inference.buffers[0] = (signed short *)malloc(n_samples * sizeof(signed short));

    if (inference.buffers[0] == NULL) {
        return false;
    }

    inference.buffers[1] = (signed short *)malloc(n_samples * sizeof(signed short));

    if (inference.buffers[0] == NULL) {
        free(inference.buffers[0]);
        return false;
    }

    sampleBuffer = (signed short *)malloc((n_samples >> 1) * sizeof(signed short));

    if (sampleBuffer == NULL) {
        free(inference.buffers[0]);
        free(inference.buffers[1]);
        return false;
    }

    inference.buf_select = 0;
    inference.buf_count = 0;
    inference.n_samples = n_samples;
    inference.buf_ready = 0;

    // configure the data receive callback
    PDM.onReceive(&pdm_data_ready_inference_callback);

    PDM.setBufferSize((n_samples >> 1) * sizeof(int16_t));

    // initialize PDM with:
    // - one channel (mono mode)
    // - a 16 kHz sample rate
    if (!PDM.begin(1, EI_CLASSIFIER_FREQUENCY)) {
        ei_printf("Failed to start PDM!");
    }

    // set the gain, defaults to 20
    PDM.setGain(127);

    record_ready = true;

    return true;
}

/**
 * @brief      Wait on new data
 *
 * @return     True when finished
 */
static bool microphone_inference_record(void)
{
    bool ret = true;

    if (inference.buf_ready == 1) {
        ei_printf(
            "Error sample buffer overrun. Decrease the number of slices per model window "
            "(EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW)\n");
        ret = false;
    }

    while (inference.buf_ready == 0) {
        delay(1);
    }

    inference.buf_ready = 0;

    return ret;
}

/**
 * Get raw audio signal data
 */
static int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr)
{
    numpy::int16_to_float(&inference.buffers[inference.buf_select ^ 1][offset], out_ptr, length);

    return 0;
}

/**
 * @brief      Stop PDM and release buffers
 */
static void microphone_inference_end(void)
{
    PDM.end();
    free(inference.buffers[0]);
    free(inference.buffers[1]);
    free(sampleBuffer);
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_MICROPHONE
#error "Invalid model for current sensor."
#endif
