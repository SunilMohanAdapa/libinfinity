/* libinfinity - a GObject-based infinote implementation
 * Copyright (C) 2007, 2008, 2009 Armin Burgmeier <armin@arbur.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <infinoted/infinoted-startup.h>
#include <infinoted/infinoted-util.h>
#include <infinoted/infinoted-creds.h>

#include <libinfinity/common/inf-cert-util.h>
#include <libinfinity/common/inf-init.h>
#include <libinfinity/common/inf-error.h>
#include <libinfinity/inf-i18n.h>
#include <libinfinity/inf-config.h>

#ifdef LIBINFINITY_HAVE_PAM
#include <security/pam_appl.h>
#endif /* LIBINFINITY_HAVE_PAM */

#include <gnutls/x509.h>

#include <string.h>
#include <stdlib.h>

static void
infinoted_startup_free_certificate_array(gnutls_x509_crt_t* certificates,
                                         guint n_certificates)
{
  guint i;
  for(i = 0; i < n_certificates; ++ i)
    gnutls_x509_crt_deinit(certificates[i]);
  g_free(certificates);
}

static gnutls_x509_privkey_t
infinoted_startup_load_key(gboolean create_key,
                           const gchar* key_file,
                           GError** error)
{
  gnutls_x509_privkey_t key;

  if(create_key == TRUE)
  {
    if(infinoted_util_create_dirname(key_file, error) == FALSE)
      return NULL;

    /* TODO: Open the key file beforehand */

    infinoted_util_log_info(_("Generating 2048 bit RSA private key..."));
    key = infinoted_creds_create_key(error);

    if(key == NULL)
      return NULL;

    if(infinoted_creds_write_key(key, key_file, error) == FALSE)
    {
      gnutls_x509_privkey_deinit(key);
      return NULL;
    }
  }
  else
  {
    key = infinoted_creds_read_key(key_file, error);
  }

  return key;
}

static gnutls_x509_crt_t*
infinoted_startup_load_certificate(gboolean create_self_signed_certificate,
                                   gnutls_x509_privkey_t key,
                                   const gchar* certificate_file,
                                   const gchar* certificate_chain_file,
                                   guint* n_certificates,
                                   GError** error)
{
  gnutls_x509_crt_t* result;
  gnutls_x509_crt_t cert;
  GPtrArray* certs;
  GPtrArray* chain_certs;

  if(create_self_signed_certificate == TRUE)
  {
    if(infinoted_util_create_dirname(certificate_file, error) == FALSE)
      return NULL;

    infinoted_util_log_info(_("Generating self-signed certificate..."));
    cert = infinoted_creds_create_self_signed_certificate(key, error);
    if(cert == NULL) return NULL;

    if(inf_cert_util_save_file(&cert, 1, certificate_file, error) == FALSE)
    {
      gnutls_x509_crt_deinit(cert);
      return NULL;
    }
    else
    {
      result = g_malloc(sizeof(gnutls_x509_crt_t));
      *result = cert;
      *n_certificates = 1;
    }
  }
  else
  {
    certs = inf_cert_util_load_file(certificate_file, NULL, error);
    if(certs == NULL) return NULL;

    if(certificate_chain_file != NULL)
    {
      chain_certs =
        inf_cert_util_load_file(certificate_chain_file, certs, error);

      if(chain_certs == NULL)
      {
        result = (gnutls_x509_crt_t*)g_ptr_array_free(certs, FALSE);
        infinoted_startup_free_certificate_array(result, *n_certificates);
        return NULL;
      }
    }

    *n_certificates = certs->len;
    result = (gnutls_x509_crt_t*)g_ptr_array_free(certs, FALSE);
  }

  return result;
}

static gboolean
infinoted_startup_load_credentials(InfinotedStartup* startup,
                                   GError** error)
{
  if(startup->options->security_policy !=
     INF_XMPP_CONNECTION_SECURITY_ONLY_UNSECURED)
  {
    startup->private_key = infinoted_startup_load_key(
      startup->options->create_key,
      startup->options->key_file,
      error
    );

    if(startup->private_key == NULL)
      return FALSE;

    startup->certificates = infinoted_startup_load_certificate(
      startup->options->create_certificate,
      startup->private_key,
      startup->options->certificate_file,
      startup->options->certificate_chain_file,
      &startup->n_certificates,
      error
    );

    if(startup->certificates == NULL)
      return FALSE;

    startup->credentials = infinoted_creds_create_credentials(
      startup->private_key,
      startup->certificates,
      startup->n_certificates,
      error
    );

    if(startup->credentials == NULL)
      return FALSE;
  }

  return TRUE;
}

static gboolean
infinoted_startup_load_options(InfinotedStartup* startup,
                               int* argc,
                               char*** argv,
                               GError** error)
{
  const gchar* const* system_config_dirs;
  guint n_system_config_dirs;
  const gchar* user_config_dir;

  const gchar* const* dir;
  guint i;

  gchar** config_files;

  system_config_dirs = g_get_system_config_dirs();
  user_config_dir = g_get_user_config_dir();

  n_system_config_dirs = 0;
  if(system_config_dirs != NULL)
  {
    for(dir = system_config_dirs; *dir != NULL; ++ dir)
      ++ n_system_config_dirs;
  }

  config_files = g_malloc( (n_system_config_dirs + 2) * sizeof(gchar*));
  config_files[n_system_config_dirs + 1] = NULL;
  config_files[n_system_config_dirs] =
    g_build_filename(user_config_dir, "infinoted.conf", NULL);

  for(i = 0; i < n_system_config_dirs; ++ i)
  {
    config_files[n_system_config_dirs - i - 1] =
      g_build_filename(system_config_dirs[i], "infinoted.conf", NULL);
  }

  startup->options =
    infinoted_options_new(
      (gchar const* const*) config_files, argc, argv, error);

  for(i = 0; i < n_system_config_dirs + 1; ++ i)
    g_free(config_files[i]);
  g_free(config_files);

  if(startup->options == NULL)
    return FALSE;

  return TRUE;
}

#ifdef LIBINFINITY_HAVE_PAM
static char*
infinoted_startup_strdup(const char* str)
{
  size_t size;
  char* new_str;

  size = strlen(str) + 1;
  new_str = malloc(size + 1);
  memcpy(new_str, str, size);

  return new_str;
}

static int
infinoted_startup_gsasl_pam_conv_func(int num_msg,
                                      const struct pam_message** msgs,
                                      struct pam_response** resps,
                                      void* appdata_ptr)
{
  int i;
  const struct pam_message* msg;
  struct pam_response* resp;

  *resps = malloc(sizeof(struct pam_response) * num_msg);

  for(i = 0; i < num_msg; ++i)
  {
    msg = msgs[i];
    resp = &(*resps)[i];
    resp->resp_retcode = 0;
    if(msg->msg_style == PAM_PROMPT_ECHO_OFF) /* looks like password prompt */
      resp->resp = infinoted_startup_strdup(appdata_ptr);
    else
      resp->resp = NULL;
  }
  return PAM_SUCCESS;
}

static void
infinoted_startup_gsasl_pam_delay_func(int retval,
                                       unsigned usec_delay,
                                       void *appdata_ptr)
{
  /* do not delay */
}

static gboolean
infinoted_startup_gsasl_pam_authenticate(const char* username,
                                         const char* password)
{
  pam_handle_t* pamh;
  struct pam_conv conv;
  void (*delay_fp)(int, unsigned, void*);
  void* delay_void_ptr;
  int status;

  conv.conv = infinoted_startup_gsasl_pam_conv_func;
  conv.appdata_ptr = *(void**) (void*) &password;

  if(pam_start("system-auth", username, &conv, &pamh) != PAM_SUCCESS)
    return FALSE;

  delay_fp = infinoted_startup_gsasl_pam_delay_func;
  /* avoid warnings for casting func-ptrs to object pointers
   * and for type-punning pointers */
  delay_void_ptr = *(void**) (void*) (char*) &delay_fp;
  status = pam_set_item(pamh, PAM_FAIL_DELAY, delay_void_ptr);
  if(status == PAM_SUCCESS)
    status = pam_authenticate(pamh, 0);

  pam_end(pamh, status);
  return status == PAM_SUCCESS;
}

#endif /* LIBINFINITY_HAVE_PAM */

static int
infinoted_startup_gsasl_callback(Gsasl* gsasl,
                                 Gsasl_session* session,
                                 Gsasl_property prop)
{
  const char* username;
  const char* password;
  InfinotedStartup* startup;

  switch(prop)
  {
  case GSASL_VALIDATE_SIMPLE:
    startup = gsasl_callback_hook_get(gsasl);
    username = gsasl_property_fast(session, GSASL_AUTHID);
    password = gsasl_property_fast(session, GSASL_PASSWORD);
#ifdef LIBINFINITY_HAVE_PAM
    if(startup->options->use_pam)
    {
      if(infinoted_startup_gsasl_pam_authenticate(username, password))
        return GSASL_OK;
      else
        return GSASL_AUTHENTICATION_ERROR;
    }
#endif /* LIBINFINITY_HAVE_PAM */
    g_assert(startup->options->password != NULL);
    if(strcmp(startup->options->password, password) == 0)
      return GSASL_OK;
    else
      return GSASL_AUTHENTICATION_ERROR;
  default:
    return GSASL_AUTHENTICATION_ERROR;
  }
}

static gboolean
infinoted_startup_load(InfinotedStartup* startup,
                       int* argc,
                       char*** argv,
                       GError** error)
{
  int gsasl_status;
  gboolean requires_password;

  if(infinoted_startup_load_options(startup, argc, argv, error) == FALSE)
    return FALSE;

  if(infinoted_startup_load_credentials(startup, error) == FALSE)
    return FALSE;

  requires_password = startup->options->password != NULL;
#ifdef LIBINFINITY_HAVE_PAM
  requires_password = requires_password || startup->options->use_pam;
#endif /* LIBINFINITY_HAVE_PAM */
  if(requires_password)
  {
    gsasl_status = gsasl_init(&startup->gsasl);
    if (gsasl_status != GSASL_OK)
    {
      inf_gsasl_set_error(error, gsasl_status);
      return FALSE;
    }
    gsasl_callback_set(startup->gsasl, infinoted_startup_gsasl_callback);
    gsasl_callback_hook_set(startup->gsasl, startup);
  }

  return TRUE;
}

/**
 * infinoted_startup_new:
 * @error: Location to store error information, if any.
 *
 * Creates parameters for starting an infinote daemon. This involves option
 * parsing, reading config files, reading or creating data for TLS
 * (private key and certificate).
 *
 * Returns: A new #InfinotedStartup. Free with infinoted_startup_free().
 */
InfinotedStartup*
infinoted_startup_new(int* argc,
                      char*** argv,
                      GError** error)
{
  InfinotedStartup* startup;

  if(!inf_init(error))
    return NULL;

  startup = g_slice_new(InfinotedStartup);
  startup->options = NULL;
  startup->private_key = NULL;
  startup->certificates = NULL;
  startup->n_certificates = 0;
  startup->credentials = NULL;
  startup->gsasl = NULL;

  if(infinoted_startup_load(startup, argc, argv, error) == FALSE)
  {
    infinoted_startup_free(startup);
    return NULL;
  }

  return startup;
}

/**
 * infinoted_startup_free:
 * @startup: A #InfinotedStartup.
 *
 * Frees all ressources allocated by @startup.
 */
void
infinoted_startup_free(InfinotedStartup* startup)
{
  if(startup->credentials != NULL)
    inf_certificate_credentials_unref(startup->credentials);

  if(startup->certificates != NULL)
  {
    infinoted_startup_free_certificate_array(
      startup->certificates,
      startup->n_certificates
    );
  }

  if(startup->private_key != NULL)
    gnutls_x509_privkey_deinit(startup->private_key);

  if(startup->options != NULL)
    infinoted_options_free(startup->options);

  if(startup->gsasl != NULL)
    gsasl_done(startup->gsasl);

  g_slice_free(InfinotedStartup, startup);
  inf_deinit();
}

/* vim:set et sw=2 ts=2: */
