/*********************************************************************
 *        _       _         _
 *  _ __ | |_  _ | |  __ _ | |__   ___
 * | '__|| __|(_)| | / _` || '_ \ / __|
 * | |   | |_  _ | || (_| || |_) |\__ \
 * |_|    \__|(_)|_| \__,_||_.__/ |___/
 *
 * www.rt-labs.com
 * Copyright 2021 rt-labs AB, Sweden.
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 ********************************************************************/

#include "app_data.h"
#include "app_utils.h"
#include "app_gsdml.h"
#include "app_log.h"
#include "sampleapp_common.h"
#include "osal.h"
#include "pnal.h"
#include <pnet_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define APP_DATA_DEFAULT_OUTPUT_DATA 0

/* Parameter data for digital submodules
 * The stored value is shared between all digital submodules in this example.
 *
 * Todo: Data is always in pnio data format. Add conversion to uint32_t.
 */
static uint32_t app_param_1 = 0; /* Network endianness */
static uint32_t app_param_2 = 0; /* Network endianness */

/* Parameter data for echo submodules
 * The stored value is shared between all echo submodules in this example.
 *
 * Todo: Data is always in pnio data format. Add conversion to uint32_t.
 */
static uint32_t app_param_echo_gain = 1; /* Network endianness */

/* Digital submodule process data
 * The stored value is shared between all digital submodules in this example. */
static uint8_t inputdata[APP_GSDML_INPUT_DATA_DIGITAL_SIZE] = {0};
static uint8_t outputdata[APP_GSDML_OUTPUT_DATA_DIGITAL_SIZE] = {0};

/**
 * Le o byte de status do sistema a partir de /tmp/pnet_status
 * Escrito pelo script pnet-status-update.sh via systemd timer a cada 5s
 *
 * Byte 0 bits:
 *   Bit 0 (0x01): WiFi (wlan0) conectado com IP
 *   Bit 1 (0x02): ZeroTier daemon ONLINE
 *   Bit 2 (0x04): ZeroTier rede b9a18a606f6713e2 = OK
 *   Bit 3 (0x08): SoftEther VPN server ativo
 *   Bit 4 (0x10): eth0 com IP (rede PROFINET)
 *   Bit 5 (0x20): Internet acessivel (ping 8.8.8.8)
 *   Bit 6 (0x40): ZeroTier IP atribuido (10.x.x.x)
 *   Bit 7 (0x80): reservado
 */
static uint8_t rls_read_status (void)
{
   FILE * f = fopen ("/tmp/pnet_status", "r");
   if (f == NULL)
   {
      return 0;
   }
   int val = 0;
   fscanf (f, "%d", &val);
   fclose (f);
   return (uint8_t)(val & 0xFF);
}

/* Network endianness */
static uint8_t echo_inputdata[APP_GSDML_INPUT_DATA_ECHO_SIZE] = {0};
static uint8_t echo_outputdata[APP_GSDML_OUTPUT_DATA_ECHO_SIZE] = {0};

/* RLS MiniPC - dados do monitor e controlo */
static uint8_t rls_monitor_data[APP_GSDML_INPUT_DATA_MONITOR_SIZE] = {0};
static uint8_t rls_control_data[APP_GSDML_OUTPUT_DATA_CONTROL_SIZE] = {0};
static uint8_t rls_control_prev[APP_GSDML_OUTPUT_DATA_CONTROL_SIZE] = {0};

/**
 * Le o estado completo do sistema a partir de /tmp/pnet_status_full
 * Formato: STATUS_FLAGS WIFI_IDX ETH0_1 ETH0_2 ETH0_3 ETH0_4 SE_SESSIONS WIFI_VISIBLE
 * Escrito pelo script pnet-status-update.sh a cada 5 segundos
 */
static void rls_update_monitor_data (void)
{
   /* Rate limiting: actualizar no maximo uma vez por segundo.
    * O ciclo PROFINET e ~1ms - sem este guarda abriamos o ficheiro
    * 1000 vezes por segundo desnecessariamente. */
   static time_t last_read = 0;
   time_t now = time (NULL);
   if (now == last_read)
      return;
   last_read = now;

   FILE * f = fopen ("/tmp/pnet_status_full", "r");
   if (f == NULL)
      return; /* Ficheiro ainda nao existe - manter valores anteriores */

   int status_flags = 0, wifi_idx = 0;
   int se_sessions = 0, wifi_visible = 0;

   int n = fscanf (f, "%d %d %d %d",
      &status_flags, &wifi_idx,
      &se_sessions, &wifi_visible);
   fclose (f);

   if (n != 4)
      return; /* Ficheiro incompleto/corrompido - manter valores anteriores */

   rls_monitor_data[0] = (uint8_t)status_flags;
   rls_monitor_data[1] = (uint8_t)wifi_idx;
   rls_monitor_data[2] = (uint8_t)se_sessions;
   rls_monitor_data[3] = (uint8_t)wifi_visible;
}

/**
 * Escreve os dados de controlo em /tmp/pnet_cmd para o script executar
 * Formato: CMD_FLAGS WIFI_IDX IP1 IP2 IP3 IP4
 */
static void rls_write_command (void)
{
   FILE * f = fopen ("/tmp/pnet_cmd", "w");
   if (f == NULL)
      return;
   fprintf (f, "%d %d\n",
      (int)rls_control_data[0],
      (int)rls_control_data[1]);
   fclose (f);
}

CC_PACKED_BEGIN
typedef struct CC_PACKED app_echo_data
{
   /* Network endianness.
      Used as a float, but we model it as a 4-byte integer to easily
      do endianness conversion */
   uint32_t echo_float_bytes;

   /* Network endianness */
   uint32_t echo_int;
} app_echo_data_t;
CC_PACKED_END
CC_STATIC_ASSERT (sizeof (app_echo_data_t) == APP_GSDML_INPUT_DATA_ECHO_SIZE);
CC_STATIC_ASSERT (sizeof (app_echo_data_t) == APP_GSDML_OUTPUT_DATA_ECHO_SIZE);

/**
 * Set LED state.
 *
 * Compares new state with previous state, to minimize system calls.
 *
 * Uses the hardware specific app_set_led() function.
 *
 * @param led_state        In:    New LED state
 */
static void app_handle_data_led_state (bool led_state)
{
   static bool previous_led_state = false;

   if (led_state != previous_led_state)
   {
      app_set_led (APP_DATA_LED_ID, led_state);
   }
   previous_led_state = led_state;
}

uint8_t * app_data_get_input_data (
   uint16_t slot_nbr,
   uint16_t subslot_nbr,
   uint32_t submodule_id,
   bool button_pressed,
   uint16_t * size,
   uint8_t * iops)
{
   float inputfloat;
   float outputfloat;
   uint32_t hostorder_inputfloat_bytes;
   uint32_t hostorder_outputfloat_bytes;
   app_echo_data_t * p_echo_inputdata = (app_echo_data_t *)&echo_inputdata;
   app_echo_data_t * p_echo_outputdata = (app_echo_data_t *)&echo_outputdata;

   if (size == NULL || iops == NULL)
   {
      return NULL;
   }

   if (
      submodule_id == APP_GSDML_SUBMOD_ID_DIGITAL_IN ||
      submodule_id == APP_GSDML_SUBMOD_ID_DIGITAL_IN_OUT)
   {
      /* Byte 0: status do sistema (WiFi, ZeroTier, SoftEther, eth0, Internet)
       * Byte 1: reservado (0x00)
       * Ver rls_read_status() para mapeamento dos bits
       */
      inputdata[0] = rls_read_status();
      inputdata[1] = 0x00;

      *size = APP_GSDML_INPUT_DATA_DIGITAL_SIZE;
      *iops = PNET_IOXS_GOOD;
      return inputdata;
   }

   if (submodule_id == APP_GSDML_SUBMOD_ID_RLS_MONITOR)
   {
      rls_update_monitor_data();
      *size = APP_GSDML_INPUT_DATA_MONITOR_SIZE;
      *iops = PNET_IOXS_GOOD;
      return rls_monitor_data;
   }

   if (submodule_id == APP_GSDML_SUBMOD_ID_ECHO)
   {
      /* Calculate echodata input (to the PLC)
       * by multiplying the output (from the PLC) with a gain factor
       */

      /* Integer */
      p_echo_inputdata->echo_int = CC_TO_BE32 (
         CC_FROM_BE32 (p_echo_outputdata->echo_int) *
         CC_FROM_BE32 (app_param_echo_gain));

      /* Float */
      /* Use memcopy to avoid strict-aliasing rule warnings */
      hostorder_outputfloat_bytes =
         CC_FROM_BE32 (p_echo_outputdata->echo_float_bytes);
      memcpy (&outputfloat, &hostorder_outputfloat_bytes, sizeof (outputfloat));
      inputfloat = outputfloat * CC_FROM_BE32 (app_param_echo_gain);
      memcpy (&hostorder_inputfloat_bytes, &inputfloat, sizeof (outputfloat));
      p_echo_inputdata->echo_float_bytes =
         CC_TO_BE32 (hostorder_inputfloat_bytes);

      *size = APP_GSDML_INPUT_DATA_ECHO_SIZE;
      *iops = PNET_IOXS_GOOD;
      return echo_inputdata;
   }

   /* Automated RT Tester scenario 2 - unsupported (sub)module */
   return NULL;
}

int app_data_set_output_data (
   uint16_t slot_nbr,
   uint16_t subslot_nbr,
   uint32_t submodule_id,
   uint8_t * data,
   uint16_t size)
{
   bool led_state;

   if (data == NULL)
   {
      return -1;
   }

   if (
      submodule_id == APP_GSDML_SUBMOD_ID_DIGITAL_OUT ||
      submodule_id == APP_GSDML_SUBMOD_ID_DIGITAL_IN_OUT)
   {
      if (size == APP_GSDML_OUTPUT_DATA_DIGITAL_SIZE)
      {
         memcpy (outputdata, data, size);
         printf ("PLC enviou: 0x%02X 0x%02X\n", outputdata[0], outputdata[1]);
         fflush(stdout);

         /* Most significant bit: LED */
         led_state = (outputdata[0] & 0x80) > 0;
         app_handle_data_led_state (led_state);

         return 0;
      }
   }
   else if (submodule_id == APP_GSDML_SUBMOD_ID_ECHO)
   {
      if (size == APP_GSDML_OUTPUT_DATA_ECHO_SIZE)
      {
         memcpy (echo_outputdata, data, size);
         return 0;
      }
   }
   else if (submodule_id == APP_GSDML_SUBMOD_ID_RLS_CONTROL)
   {
      if (size == APP_GSDML_OUTPUT_DATA_CONTROL_SIZE)
      {
         memcpy (rls_control_data, data, size);
         /* Detectar mudanca nos dados e escrever comando */
         if (memcmp (rls_control_data, rls_control_prev, size) != 0)
         {
            memcpy (rls_control_prev, rls_control_data, size);
            rls_write_command();
         }
         return 0;
      }
   }

   return -1;
}

int app_data_set_default_outputs (void)
{
   outputdata[0] = APP_DATA_DEFAULT_OUTPUT_DATA;
   app_handle_data_led_state (false);
   return 0;
}

int app_data_write_parameter (
   uint16_t slot_nbr,
   uint16_t subslot_nbr,
   uint32_t submodule_id,
   uint32_t index,
   const uint8_t * data,
   uint16_t length)
{
   const app_gsdml_param_t * par_cfg;

   par_cfg = app_gsdml_get_parameter_cfg (submodule_id, index);
   if (par_cfg == NULL)
   {
      APP_LOG_WARNING (
         "PLC write request unsupported submodule/parameter. "
         "Submodule id: %u Index: %u\n",
         (unsigned)submodule_id,
         (unsigned)index);
      return -1;
   }

   if (length != par_cfg->length)
   {
      APP_LOG_WARNING (
         "PLC write request unsupported length. "
         "Index: %u Length: %u Expected length: %u\n",
         (unsigned)index,
         (unsigned)length,
         par_cfg->length);
      return -1;
   }

   if (index == APP_GSDML_PARAMETER_1_IDX)
   {
      memcpy (&app_param_1, data, length);
   }
   else if (index == APP_GSDML_PARAMETER_2_IDX)
   {
      memcpy (&app_param_2, data, length);
   }
   else if (index == APP_GSDML_PARAMETER_ECHO_IDX)
   {
      memcpy (&app_param_echo_gain, data, length);
   }

   APP_LOG_DEBUG ("  Writing parameter \"%s\"\n", par_cfg->name);
   app_log_print_bytes (APP_LOG_LEVEL_DEBUG, data, length);

   return 0;
}

int app_data_read_parameter (
   uint16_t slot_nbr,
   uint16_t subslot_nbr,
   uint32_t submodule_id,
   uint32_t index,
   uint8_t ** data,
   uint16_t * length)
{
   const app_gsdml_param_t * par_cfg;

   par_cfg = app_gsdml_get_parameter_cfg (submodule_id, index);
   if (par_cfg == NULL)
   {
      APP_LOG_WARNING (
         "PLC read request unsupported submodule/parameter. "
         "Submodule id: %u Index: %u\n",
         (unsigned)submodule_id,
         (unsigned)index);
      return -1;
   }

   if (*length < par_cfg->length)
   {
      APP_LOG_WARNING (
         "PLC read request unsupported length. "
         "Index: %u Max length: %u Data length for our parameter: %u\n",
         (unsigned)index,
         (unsigned)*length,
         par_cfg->length);
      return -1;
   }

   APP_LOG_DEBUG ("  Reading \"%s\"\n", par_cfg->name);
   if (index == APP_GSDML_PARAMETER_1_IDX)
   {
      *data = (uint8_t *)&app_param_1;
      *length = sizeof (app_param_1);
   }
   else if (index == APP_GSDML_PARAMETER_2_IDX)
   {
      *data = (uint8_t *)&app_param_2;
      *length = sizeof (app_param_2);
   }
   else if (index == APP_GSDML_PARAMETER_ECHO_IDX)
   {
      *data = (uint8_t *)&app_param_echo_gain;
      *length = sizeof (app_param_echo_gain);
   }

   app_log_print_bytes (APP_LOG_LEVEL_DEBUG, *data, *length);

   return 0;
}
