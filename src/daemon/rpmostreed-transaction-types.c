/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"
#include "ostree.h"

#include <libglnx.h>

#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-sysroot.h"
#include "rpmostree-sysroot-upgrader.h"
#include "rpmostree-sysroot-core.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"
#include "rpmostree-output.h"
#include "rpmostree-core.h"
#include "rpmostree-importer.h"
#include "rpmostreed-utils.h"
#include "rpmostree-kargs-process.h"

static gboolean
change_origin_refspec (OstreeSysroot *sysroot,
                       RpmOstreeOrigin *origin,
                       const gchar *refspec,
                       GCancellable *cancellable,
                       gchar **out_old_refspec,
                       gchar **out_new_refspec,
                       GError **error)
{
  g_autofree gchar *current_refspec =
    g_strdup (rpmostree_origin_get_refspec (origin));
  g_autofree gchar *new_refspec = NULL;
  if (!rpmostreed_refspec_parse_partial (refspec,
                                         current_refspec,
                                         &new_refspec,
                                         error))
    return FALSE;

  if (strcmp (current_refspec, new_refspec) == 0)
    return glnx_throw (error, "Old and new refs are equal: %s", new_refspec);

  if (!rpmostree_origin_set_rebase (origin, new_refspec, error))
    return FALSE;

  g_autofree gchar *current_remote = NULL;
  g_autofree gchar *current_branch = NULL;
  g_assert (ostree_parse_refspec (current_refspec, &current_remote, &current_branch, NULL));

  g_autofree gchar *new_remote = NULL;
  g_autofree gchar *new_branch = NULL;
  g_assert (ostree_parse_refspec (new_refspec, &new_remote, &new_branch, NULL));

  /* This version is a bit magical, so let's explain it.
     https://github.com/projectatomic/rpm-ostree/issues/569 */
  const gboolean switching_only_remote =
    g_strcmp0 (new_remote, current_remote) != 0 &&
    g_strcmp0 (new_branch, current_branch) == 0;
  if (switching_only_remote && new_remote != NULL)
    rpmostree_output_message ("Rebasing to %s:%s", new_remote, current_branch);

  if (out_new_refspec != NULL)
    *out_new_refspec = g_steal_pointer (&new_refspec);

  if (out_old_refspec != NULL)
    *out_old_refspec = g_strdup (current_refspec);

  return TRUE;
}

static gboolean
apply_revision_override (RpmostreedTransaction    *transaction,
                         OstreeRepo               *repo,
                         OstreeAsyncProgress      *progress,
                         RpmOstreeOrigin          *origin,
                         const char               *revision,
                         GCancellable             *cancellable,
                         GError                  **error)
{
  g_autofree char *checksum = NULL;
  g_autofree char *version = NULL;

  if (!rpmostreed_parse_revision (revision,
                                  &checksum,
                                  &version,
                                  error))
    return FALSE;

  if (version != NULL)
    {
      rpmostreed_transaction_emit_message_printf (transaction,
                                                  "Resolving version '%s'",
                                                  version);

      if (!rpmostreed_repo_lookup_version (repo, rpmostree_origin_get_refspec (origin),
                                           version, progress,
                                           cancellable, &checksum, error))
        return FALSE;
    }
  else
    {
      g_assert (checksum != NULL);

      rpmostreed_transaction_emit_message_printf (transaction,
                                                  "Validating checksum '%s'",
                                                  checksum);

      if (!rpmostreed_repo_lookup_checksum (repo, rpmostree_origin_get_refspec (origin),
                                            checksum, progress, cancellable, error))
        return FALSE;
    }

  rpmostree_origin_set_override_commit (origin, checksum, version);

  return TRUE;
}

/* ============================= Package Diff  ============================= */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  char *refspec;
  char *revision;
} PackageDiffTransaction;

typedef RpmostreedTransactionClass PackageDiffTransactionClass;

GType package_diff_transaction_get_type (void);

G_DEFINE_TYPE (PackageDiffTransaction,
               package_diff_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
package_diff_transaction_finalize (GObject *object)
{
  PackageDiffTransaction *self;

  self = (PackageDiffTransaction *) object;
  g_free (self->osname);
  g_free (self->refspec);
  g_free (self->revision);

  G_OBJECT_CLASS (package_diff_transaction_parent_class)->finalize (object);
}

static gboolean
package_diff_transaction_execute (RpmostreedTransaction *transaction,
                                  GCancellable *cancellable,
                                  GError **error)
{
  PackageDiffTransaction *self = (PackageDiffTransaction *) transaction;
  RpmOstreeSysrootUpgraderFlags upgrader_flags = 0;

  if (self->revision != NULL || self->refspec != NULL)
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER;

  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);
  g_autoptr(RpmOstreeSysrootUpgrader) upgrader =
    rpmostree_sysroot_upgrader_new (sysroot, self->osname, upgrader_flags,
                                    cancellable, error);
  if (upgrader == NULL)
    return FALSE;

  g_autoptr(RpmOstreeOrigin) origin =
    rpmostree_sysroot_upgrader_dup_origin (upgrader);

  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;

  /* Determine if we're upgrading before we set the refspec. */
  gboolean upgrading = (self->refspec == NULL && self->revision == NULL);

  if (self->refspec != NULL)
    {
      if (!change_origin_refspec (sysroot, origin, self->refspec,
                                  cancellable, NULL, NULL, error))
        return FALSE;
    }

  g_autoptr(OstreeAsyncProgress) progress =
    ostree_async_progress_new ();
  rpmostreed_transaction_connect_download_progress (transaction, progress);
  rpmostreed_transaction_connect_signature_progress (transaction, repo);

  if (self->revision != NULL)
    {
      if (!apply_revision_override (transaction, repo, progress, origin,
                                    self->revision, cancellable, error))
        return FALSE;
    }
  else if (upgrading)
    {
      rpmostree_origin_set_override_commit (origin, NULL, NULL);
    }

  rpmostree_sysroot_upgrader_set_origin (upgrader, origin);

  if (self->refspec != NULL)
    {
      rpmostreed_transaction_emit_message_printf (transaction,
                                                  "Updating from: %s",
                                                  self->refspec);
    }

  gboolean changed = FALSE;
  if (!rpmostree_sysroot_upgrader_pull_base (upgrader, "/usr/share/rpm",
                                             0, progress, &changed,
                                             cancellable, error))
    return FALSE;

  rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));

  if (!changed)
    {
      if (upgrading)
        rpmostreed_transaction_emit_message_printf (transaction,
                                                    "No upgrade available.");
      else
        rpmostreed_transaction_emit_message_printf (transaction,
                                                    "No change.");
    }

  return TRUE;
}

static void
package_diff_transaction_class_init (PackageDiffTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = package_diff_transaction_finalize;

  class->execute = package_diff_transaction_execute;
}

static void
package_diff_transaction_init (PackageDiffTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_package_diff (GDBusMethodInvocation *invocation,
                                         OstreeSysroot *sysroot,
                                         const char *osname,
                                         const char *refspec,
                                         const char *revision,
                                         GCancellable *cancellable,
                                         GError **error)
{
  PackageDiffTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);

  self = g_initable_new (package_diff_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->refspec = g_strdup (refspec);
      self->revision = g_strdup (revision);
    }

  return (RpmostreedTransaction *) self;
}

/* =============================== Rollback =============================== */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  gboolean reboot;
} RollbackTransaction;

typedef RpmostreedTransactionClass RollbackTransactionClass;

GType rollback_transaction_get_type (void);

G_DEFINE_TYPE (RollbackTransaction,
               rollback_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
rollback_transaction_finalize (GObject *object)
{
  RollbackTransaction *self;

  self = (RollbackTransaction *) object;
  g_free (self->osname);

  G_OBJECT_CLASS (rollback_transaction_parent_class)->finalize (object);
}

static gboolean
rollback_transaction_execute (RpmostreedTransaction *transaction,
                              GCancellable *cancellable,
                              GError **error)
{
  RollbackTransaction *self = (RollbackTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  g_autoptr(OstreeDeployment) pending_deployment = NULL;
  g_autoptr(OstreeDeployment) rollback_deployment = NULL;
  ostree_sysroot_query_deployments_for (sysroot, self->osname,
                                        &pending_deployment, &rollback_deployment);

  if (!rollback_deployment && !pending_deployment) /* i.e. do we just have 1 deployment? */
    return glnx_throw (error, "No rollback deployment found");
  else if (!rollback_deployment)
    {
      /* If there isn't a rollback deployment, but there *is* a pending deployment, then we
       * want "rpm-ostree rollback" to put the currently booted deployment back on top. This
       * also allows users to effectively undo a rollback operation. */
      rollback_deployment = g_object_ref (booted_deployment);
    }

  g_autoptr(GPtrArray) old_deployments =
    ostree_sysroot_get_deployments (sysroot);
  g_autoptr(GPtrArray) new_deployments =
    g_ptr_array_new_with_free_func (g_object_unref);

  /* build out the reordered array; rollback is first now */
  g_ptr_array_add (new_deployments, g_object_ref (rollback_deployment));

  rpmostreed_transaction_emit_message_printf (transaction,
                                              "Moving '%s.%d' to be first deployment",
                                              ostree_deployment_get_csum (rollback_deployment),
                                              ostree_deployment_get_deployserial (rollback_deployment));

  for (guint i = 0; i < old_deployments->len; i++)
    {
      OstreeDeployment *deployment = old_deployments->pdata[i];
      if (!ostree_deployment_equal (deployment, rollback_deployment))
        g_ptr_array_add (new_deployments, g_object_ref (deployment));
    }

  /* if default changed write it */
  if (old_deployments->pdata[0] != new_deployments->pdata[0])
    {
      if (!ostree_sysroot_write_deployments (sysroot,
                                             new_deployments,
                                             cancellable,
                                             error))
        return FALSE;
    }

  if (self->reboot)
    rpmostreed_reboot (cancellable, error);

  return TRUE;
}

static void
rollback_transaction_class_init (RollbackTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = rollback_transaction_finalize;

  class->execute = rollback_transaction_execute;
}

static void
rollback_transaction_init (RollbackTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_rollback (GDBusMethodInvocation *invocation,
                                     OstreeSysroot *sysroot,
                                     const char *osname,
                                     gboolean reboot,
                                     GCancellable *cancellable,
                                     GError **error)
{
  RollbackTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);

  self = g_initable_new (rollback_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->reboot = reboot;
    }

  return (RpmostreedTransaction *) self;
}

/* ============================ UpdateDeployment ============================ */

typedef struct {
  RpmostreedTransaction parent;
  RpmOstreeTransactionDeployFlags flags;
  char   *osname;
  char   *refspec; /* NULL for non-rebases */
  char   *revision; /* NULL for upgrade */
  char  **install_pkgs;
  GUnixFDList *install_local_pkgs;
  char  **uninstall_pkgs;
  char  **override_replace_pkgs;
  GUnixFDList *override_replace_local_pkgs;
  char  **override_remove_pkgs;
  char  **override_reset_pkgs;
} DeployTransaction;

typedef RpmostreedTransactionClass DeployTransactionClass;

GType deploy_transaction_get_type (void);

G_DEFINE_TYPE (DeployTransaction,
               deploy_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
deploy_transaction_finalize (GObject *object)
{
  DeployTransaction *self;

  self = (DeployTransaction *) object;
  g_free (self->osname);
  g_free (self->refspec);
  g_free (self->revision);
  g_strfreev (self->install_pkgs);
  g_clear_pointer (&self->install_local_pkgs, g_object_unref);
  g_strfreev (self->uninstall_pkgs);
  g_strfreev (self->override_replace_pkgs);
  g_clear_pointer (&self->override_replace_local_pkgs, g_object_unref);
  g_strfreev (self->override_remove_pkgs);
  g_strfreev (self->override_reset_pkgs);

  G_OBJECT_CLASS (deploy_transaction_parent_class)->finalize (object);
}

static gboolean
import_local_rpm (OstreeRepo    *repo,
                  int           *fd,
                  char         **sha256_nevra,
                  GCancellable  *cancellable,
                  GError       **error)
{
  /* let's just use the current sepolicy -- we'll just relabel it if the new
   * base turns out to have a different one */
  glnx_autofd int rootfs_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, "/", TRUE, &rootfs_dfd, error))
    return FALSE;
  g_autoptr(OstreeSePolicy) policy = ostree_sepolicy_new_at (rootfs_dfd, cancellable, error);
  if (policy == NULL)
    return FALSE;

  g_autoptr(RpmOstreeImporter) unpacker = rpmostree_importer_new_take_fd (fd, repo, NULL, 0, policy, error);
  if (unpacker == NULL)
    return FALSE;

  if (!rpmostree_importer_run (unpacker, NULL, cancellable, error))
    return FALSE;

  g_autofree char *nevra = rpmostree_importer_get_nevra (unpacker);
  *sha256_nevra = g_strconcat (rpmostree_importer_get_header_sha256 (unpacker),
                               ":", nevra, NULL);

  return TRUE;
}

static void
ptr_close_fd (gpointer fdp)
{
  int fd = GPOINTER_TO_INT (fdp);
  glnx_close_fd (&fd);
}

/* GUnixFDList doesn't allow stealing individual members */
static GPtrArray *
unixfdlist_to_ptrarray (GUnixFDList *fdl)
{
  gint len;
  gint *fds = g_unix_fd_list_steal_fds (fdl, &len);
  GPtrArray *ret = g_ptr_array_new_with_free_func ((GDestroyNotify)ptr_close_fd);
  for (int i = 0; i < len; i++)
    g_ptr_array_add (ret, GINT_TO_POINTER (fds[i]));
  return ret;
}

static gboolean
import_many_local_rpms (OstreeRepo    *repo,
                        GUnixFDList   *fdl,
                        GPtrArray    **out_pkgs,
                        GCancellable  *cancellable,
                        GError       **error)
{
  /* Note that we record the SHA-256 of the RPM header in the origin to make sure that e.g.
   * if we somehow re-import the same NEVRA with different content, we error out. We don't
   * record the checksum of the branch itself, because it may need relabeling and that's OK.
   * */

  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  /* Note use of commit-on-failure */
  if (!rpmostree_repo_auto_transaction_start (&txn, repo, TRUE, cancellable, error))
    return FALSE;

  g_autoptr(GPtrArray) pkgs = g_ptr_array_new_with_free_func (g_free);

  g_autoptr(GPtrArray) fds = unixfdlist_to_ptrarray (fdl);
  for (guint i = 0; i < fds->len; i++)
    {
      /* Steal fd from the ptrarray */
      glnx_autofd int fd = GPOINTER_TO_INT (fds->pdata[i]);
      fds->pdata[i] = GINT_TO_POINTER (-1);
      g_autofree char *sha256_nevra = NULL;
      /* Transfer fd to import */
      if (!import_local_rpm (repo, &fd, &sha256_nevra, cancellable, error))
        return FALSE;

      g_ptr_array_add (pkgs, g_steal_pointer (&sha256_nevra));
    }

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;
  txn.initialized = FALSE;

  *out_pkgs = g_steal_pointer (&pkgs);
  return TRUE;
}

static void
gv_nevra_add_nevra_name_mappings (GVariant *gv_nevra,
                                  GHashTable *name_to_nevra,
                                  GHashTable *nevra_to_name)
{
  const char *name = NULL;
  const char *nevra = NULL;
  g_variant_get_child (gv_nevra, 0, "&s", &nevra);
  g_variant_get_child (gv_nevra, 1, "&s", &name);
  g_hash_table_insert (name_to_nevra, (gpointer)name, (gpointer)nevra);
  g_hash_table_insert (nevra_to_name, (gpointer)nevra, (gpointer)name);
}

static gboolean
deploy_transaction_execute (RpmostreedTransaction *transaction,
                            GCancellable *cancellable,
                            GError **error)
{
  DeployTransaction *self = (DeployTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);

  const gboolean dry_run =
    ((self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DRY_RUN) > 0);
  const gboolean no_overrides =
    ((self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NO_OVERRIDES) > 0);

  RpmOstreeSysrootUpgraderFlags upgrader_flags = 0;
  if (self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE)
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER;
  if (dry_run)
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN;

  if (self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_CACHE_ONLY)
    {
      /* practically, we could unite those two into a single flag, though it's nice to be
       * able to keep them separate as well */

      /* don't pull, just resolve ref locally and timestamp check */
      upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_SYNTHETIC_PULL;
      /* turn on rpmmd cache only in the upgrader */
      upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGCACHE_ONLY;
    }

  /* this should have been checked already */
  if (no_overrides)
    {
      g_assert (self->override_replace_pkgs == NULL);
      g_assert (self->override_replace_local_pkgs == NULL);
      g_assert (self->override_remove_pkgs == NULL);
      g_assert (self->override_reset_pkgs == NULL);
    }

  if (self->refspec)
    {
      /* When rebasing, we should be able to switch to a different tree even if
       * the current origin is unconfigured */
      upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED;
    }

  /* before doing any real work, let's make sure the pkgcache is migrated */
  OstreeRepo *repo = ostree_sysroot_repo (sysroot);
  if (!rpmostree_migrate_pkgcache_repo (repo, cancellable, error))
    return FALSE;

  g_autoptr(RpmOstreeSysrootUpgrader) upgrader =
    rpmostree_sysroot_upgrader_new (sysroot, self->osname, upgrader_flags,
                                    cancellable, error);
  if (upgrader == NULL)
    return FALSE;

  g_autoptr(RpmOstreeOrigin) origin =
    rpmostree_sysroot_upgrader_dup_origin (upgrader);

  g_autofree gchar *new_refspec = NULL;
  g_autofree gchar *old_refspec = NULL;
  if (self->refspec)
    {
      if (!change_origin_refspec (sysroot, origin, self->refspec, cancellable,
                                  &old_refspec, &new_refspec, error))
        return FALSE;
    }

  g_autoptr(OstreeAsyncProgress) progress =
    ostree_async_progress_new ();

  rpmostreed_transaction_connect_download_progress (transaction, progress);
  rpmostreed_transaction_connect_signature_progress (transaction, repo);

  if (self->revision)
    {
      if (!apply_revision_override (transaction, repo, progress, origin,
                                    self->revision, cancellable, error))
        return FALSE;
    }
  else
    {
      rpmostree_origin_set_override_commit (origin, NULL, NULL);
    }

  gboolean is_install = FALSE;
  gboolean is_override = FALSE;

  /* In practice today */
  if (self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NO_PULL_BASE)
    {
      /* this is a heuristic; by the end, once the proper switches are added, the two
       * commands can look indistinguishable at the D-Bus level */
      is_override = (self->override_reset_pkgs ||
                     self->override_remove_pkgs ||
                     self->override_replace_pkgs ||
                     self->override_replace_local_pkgs ||
                     no_overrides);
      is_install = !is_override;
    }

  /* https://github.com/projectatomic/rpm-ostree/issues/454 */
  g_autoptr(GString) txn_title = g_string_new ("");
  if (is_install)
    g_string_append (txn_title, "install");
  else if (is_override)
    g_string_append (txn_title, "override");
  else if (self->refspec)
    g_string_append (txn_title, "rebase");
  else if (self->revision)
    g_string_append (txn_title, "deploy");
  else
    g_string_append (txn_title, "upgrade");

  gboolean changed = FALSE;
  if (self->uninstall_pkgs)
    {
      if (!rpmostree_origin_remove_packages (origin, self->uninstall_pkgs, error))
        return FALSE;

      /* in reality, there may not be any new layer required (if e.g. we're
       * removing a duplicate provides), though the origin has changed so we
       * need to create a new deployment -- see also
       * https://github.com/projectatomic/rpm-ostree/issues/753 */
      changed = TRUE;

      g_string_append_printf (txn_title, "; uninstall: %u",
                              g_strv_length (self->uninstall_pkgs));
    }

  if (self->install_pkgs)
    {
      if (!rpmostree_origin_add_packages (origin, self->install_pkgs, FALSE, error))
        return FALSE;

      /* here too -- we could optimize this under certain conditions
       * (see related blurb in maybe_do_local_assembly()) */
      changed = TRUE;

      g_string_append_printf (txn_title, "; install: %u",
                              g_strv_length (self->install_pkgs));
    }

  if (self->install_local_pkgs != NULL)
    {
      g_autoptr(GPtrArray) pkgs = NULL;
      if (!import_many_local_rpms (repo, self->install_local_pkgs, &pkgs,
                                   cancellable, error))
        return FALSE;

      if (pkgs->len > 0)
        {
          g_ptr_array_add (pkgs, NULL);
          if (!rpmostree_origin_add_packages (origin, (char**)pkgs->pdata, TRUE, error))
            return FALSE;

          changed = TRUE;
          g_string_append_printf (txn_title, "; localinstall: %u", pkgs->len);
        }
    }

  if (no_overrides)
    {
      gboolean overrides_changed = FALSE;
      if (!rpmostree_origin_remove_all_overrides (origin, &overrides_changed, error))
        return FALSE;

      changed = changed || overrides_changed;
    }
  else if (self->override_reset_pkgs)
    {
      /* The origin stores removal overrides as pkgnames and replacement overrides as nevra.
       * To be nice, we support both name & nevra and do the translation here by just
       * looking at the commit metadata. */
      OstreeDeployment *merge_deployment =
        rpmostree_sysroot_upgrader_get_merge_deployment (upgrader);

      gboolean is_layered;
      g_autoptr(GVariant) removed = NULL;
      g_autoptr(GVariant) replaced = NULL;
      if (!rpmostree_deployment_get_layered_info (repo, merge_deployment, &is_layered, NULL,
                                                  NULL, &removed, &replaced, error))
        return FALSE;

      if (!is_layered)
        return glnx_throw (error, "No overrides currently applied");

      g_autoptr(GHashTable) nevra_to_name = g_hash_table_new (g_str_hash, g_str_equal);
      g_autoptr(GHashTable) name_to_nevra = g_hash_table_new (g_str_hash, g_str_equal);

      /* keep a reference on the child nevras so that the hash tables above can directly
       * reference strings within them */
      g_autoptr(GPtrArray) gv_nevras =
        g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

      const guint nremoved = g_variant_n_children (removed);
      for (guint i = 0; i < nremoved; i++)
        {
          g_autoptr(GVariant) gv_nevra;
          g_variant_get_child (removed, i, "v", &gv_nevra);
          gv_nevra_add_nevra_name_mappings (gv_nevra, name_to_nevra, nevra_to_name);
          g_ptr_array_add (gv_nevras, g_steal_pointer (&gv_nevra));
        }

      const guint nreplaced = g_variant_n_children (replaced);
      for (guint i = 0; i < nreplaced; i++)
        {
          g_autoptr(GVariant) gv_nevra;
          g_variant_get_child (replaced, i, "(vv)", &gv_nevra, NULL);
          gv_nevra_add_nevra_name_mappings (gv_nevra, name_to_nevra, nevra_to_name);
          g_ptr_array_add (gv_nevras, g_steal_pointer (&gv_nevra));
        }

      for (char **it = self->override_reset_pkgs; it && *it; it++)
        {
          const char *name_or_nevra = *it;
          const char *name = g_hash_table_lookup (nevra_to_name, name_or_nevra);
          const char *nevra = g_hash_table_lookup (name_to_nevra, name_or_nevra);

          if (name == NULL && nevra == NULL)
            return glnx_throw (error, "No overrides for package '%s'", name_or_nevra);
          else if (name == NULL)
            name = name_or_nevra;
          else if (nevra == NULL)
            nevra = name_or_nevra;
          else
            {
              /* completely brush over the ridiculous corner-case of a
                 pkgname that's also a nevra for another package */
              g_assert_not_reached ();
            }

          if (rpmostree_origin_remove_override (origin, name,
                                                RPMOSTREE_ORIGIN_OVERRIDE_REMOVE))
            continue; /* override found; move on to the next one */

          if (rpmostree_origin_remove_override (origin, nevra,
                                                RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL))
            continue; /* override found; move on to the next one */

          /* if a mapping was found, then it must be an override */
          g_assert_not_reached ();
        }

      changed = TRUE;
    }

  if (self->override_replace_local_pkgs)
    {
      g_autoptr(GPtrArray) pkgs = NULL;
      if (!import_many_local_rpms (repo, self->override_replace_local_pkgs, &pkgs,
                                   cancellable, error))
        return FALSE;

      if (pkgs->len > 0)
        {
          g_ptr_array_add (pkgs, NULL);
          if (!rpmostree_origin_add_overrides (origin, (char**)pkgs->pdata,
                                               RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL,
                                               error))
            return FALSE;
          changed = TRUE;
        }
    }

  rpmostree_transaction_set_title ((RPMOSTreeTransaction*)self, txn_title->str);

  rpmostree_sysroot_upgrader_set_origin (upgrader, origin);

  /* Mainly for the `install` and `override` commands */
  const gboolean no_pull_base =
    ((self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NO_PULL_BASE) > 0);

  if (!no_pull_base)
    {
      gboolean base_changed;

      if (!rpmostree_sysroot_upgrader_pull_base (upgrader, NULL, 0, progress,
                                                 &base_changed, cancellable, error))
        return FALSE;

      if (base_changed)
        changed = TRUE;
      else
        {
          /* If we're on a live deployment, then allow redeploying a clean version of the
           * same base commit. This is useful if e.g. the pushed rollback was cleaned up. */

          OstreeDeployment *deployment =
            rpmostree_sysroot_upgrader_get_merge_deployment (upgrader);

          gboolean is_live;
          if (!rpmostree_syscore_deployment_is_live (sysroot, deployment, &is_live, error))
            return FALSE;

          if (is_live)
            changed = TRUE;
        }
    }

  /* let's figure out if those new overrides are valid and if so, canonicalize
   * them -- we could have just pulled the rpmdb dir before to do this, and then
   * do the full pull afterwards, though that would complicate the pull code and
   * anyway in the common case even if there's an error with the overrides,
   * users will fix it and try again, so the second pull will be a no-op */

  if (self->override_remove_pkgs)
    {
      const char *base = rpmostree_sysroot_upgrader_get_base (upgrader);
      g_autoptr(RpmOstreeRefSack) rsack =
        rpmostree_get_refsack_for_commit (repo, base, cancellable, error);
      if (rsack == NULL)
        return FALSE;

      /* NB: the strings are owned by the sack pool */
      g_autoptr(GPtrArray) pkgnames = g_ptr_array_new ();
      for (char **it = self->override_remove_pkgs; it && *it; it++)
        {
          const char *pkg = *it;
          g_autoptr(GPtrArray) pkgs = rpmostree_get_matching_packages (rsack->sack, pkg);

          if (pkgs->len == 0)
            return glnx_throw (error, "No package \"%s\" in base commit %.7s", pkg, base);

          /* either the subject was somehow too broad, or it's one of the rare
           * packages that supports installonly (e.g. kernel, though that one
           * specifically should never have multiple instances in a compose),
           * which you'd never want to remove */
          if (pkgs->len > 1)
            return glnx_throw (error, "Multiple packages match \"%s\"", pkg);

          /* canonicalize to just the pkg name */
          const char *pkgname = dnf_package_get_name (pkgs->pdata[0]);
          g_ptr_array_add (pkgnames, (void*)pkgname);
        }

      g_ptr_array_add (pkgnames, NULL);
      if (!rpmostree_origin_add_overrides (origin, (char**)pkgnames->pdata,
                                           RPMOSTREE_ORIGIN_OVERRIDE_REMOVE, error))
        return FALSE;

      rpmostree_sysroot_upgrader_set_origin (upgrader, origin);
      changed = TRUE;
    }

  RpmOstreeSysrootUpgraderLayeringType layering_type;
  gboolean layering_changed = FALSE;
  if (!rpmostree_sysroot_upgrader_prep_layering (upgrader, &layering_type, &layering_changed,
                                                 cancellable, error))
    return FALSE;
  changed = changed || layering_changed;

  if (dry_run)
    /* Note early return here; we printed the transaction already */
    return TRUE;

  if (layering_changed)
    {
      if (!rpmostree_sysroot_upgrader_import_pkgs (upgrader, cancellable, error))
        return FALSE;
    }

  /* XXX: check if this is needed */
  rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));

  /* TODO - better logic for "changed" based on deployments */
  if (changed || self->refspec)
    {
      /* Note early return; we stop short of actually writing the deployment */
      if (self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DOWNLOAD_ONLY)
        {
          /* XXX: improve msg here; e.g. cache will be blown on next operation? */
          if (changed)
            rpmostreed_transaction_emit_message_printf (transaction, "Update downloaded.");
          else
            rpmostreed_transaction_emit_message_printf (transaction, "No changes.");
          return TRUE;
        }

      if (!rpmostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
        return FALSE;

      /* Are we rebasing?  May want to delete the previous ref */
      if (self->refspec && !(self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_SKIP_PURGE))
        {
          g_autofree char *remote = NULL;
          g_autofree char *ref = NULL;

          /* The actual rebase has already succeeded, so ignore errors. */
          if (ostree_parse_refspec (old_refspec, &remote, &ref, NULL))
            {
              /* Note: In some cases the source origin ref may not actually
               * exist; say the admin did a cleanup, or the OS expects post-
               * install configuration like subscription-manager. */
              (void) ostree_repo_set_ref_immediate (repo, remote, ref, NULL,
                                                    cancellable, NULL);
            }
        }

      if (self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_REBOOT)
        rpmostreed_reboot (cancellable, error);
    }
  else
    {
      if (!self->revision)
        rpmostreed_transaction_emit_message_printf (transaction, "No upgrade available.");
      else
        rpmostreed_transaction_emit_message_printf (transaction, "No change.");
    }

  return TRUE;
}

static void
deploy_transaction_class_init (DeployTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = deploy_transaction_finalize;

  class->execute = deploy_transaction_execute;
}

static void
deploy_transaction_init (DeployTransaction *self)
{
}

static char **
strdupv_canonicalize (const char *const *strv)
{
  if (strv && *strv)
    return g_strdupv ((char**)strv);
  return NULL;
}

RpmostreedTransaction *
rpmostreed_transaction_new_deploy (GDBusMethodInvocation *invocation,
                                   OstreeSysroot *sysroot,
                                   RpmOstreeTransactionDeployFlags flags,
                                   const char *osname,
                                   const char *refspec,
                                   const char *revision,
                                   const char *const *install_pkgs,
                                   GUnixFDList       *install_local_pkgs,
                                   const char *const *uninstall_pkgs,
                                   const char *const *override_replace_pkgs,
                                   GUnixFDList       *override_replace_local_pkgs,
                                   const char *const *override_remove_pkgs,
                                   const char *const *override_reset_pkgs,
                                   GCancellable *cancellable,
                                   GError **error)
{
  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);

  DeployTransaction *self =
    g_initable_new (deploy_transaction_get_type (),
                    cancellable, error,
                    "invocation", invocation,
                    "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                    NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->flags = flags;
      self->refspec = g_strdup (refspec);
      self->revision = g_strdup (revision);
      self->install_pkgs = strdupv_canonicalize (install_pkgs);
      if (install_local_pkgs != NULL)
        self->install_local_pkgs = g_object_ref (install_local_pkgs);
      self->uninstall_pkgs = strdupv_canonicalize (uninstall_pkgs);
      self->override_replace_pkgs = strdupv_canonicalize (override_replace_pkgs);
      if (override_replace_local_pkgs != NULL)
        self->override_replace_local_pkgs = g_object_ref (override_replace_local_pkgs);
      self->override_remove_pkgs = strdupv_canonicalize (override_remove_pkgs);
      self->override_reset_pkgs = strdupv_canonicalize (override_reset_pkgs);
    }

  return (RpmostreedTransaction *) self;
}

/* ================================ InitramfsState ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  gboolean regenerate;
  char **args;
  gboolean reboot;
} InitramfsStateTransaction;

typedef RpmostreedTransactionClass InitramfsStateTransactionClass;

GType initramfs_state_transaction_get_type (void);

G_DEFINE_TYPE (InitramfsStateTransaction,
               initramfs_state_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
initramfs_state_transaction_finalize (GObject *object)
{
  InitramfsStateTransaction *self;

  self = (InitramfsStateTransaction *) object;
  g_free (self->osname);
  g_strfreev (self->args);

  G_OBJECT_CLASS (initramfs_state_transaction_parent_class)->finalize (object);
}

static gboolean
initramfs_state_transaction_execute (RpmostreedTransaction *transaction,
                            GCancellable *cancellable,
                            GError **error)
{

  InitramfsStateTransaction *self = (InitramfsStateTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);

  g_autoptr(RpmOstreeSysrootUpgrader) upgrader =
    rpmostree_sysroot_upgrader_new (sysroot, self->osname, 0,
                                    cancellable, error);
  if (upgrader == NULL)
    return FALSE;

  g_autoptr(RpmOstreeOrigin) origin = rpmostree_sysroot_upgrader_dup_origin (upgrader);
  gboolean current_regenerate = rpmostree_origin_get_regenerate_initramfs (origin);
  const char *const* current_initramfs_args = rpmostree_origin_get_initramfs_args (origin);

  /* We don't deep-compare the args right now, we assume if you were using them
   * you want to rerun. This can be important if you edited a config file, which
   * we can't really track without actually regenerating anyways.
   */
  if (current_regenerate == self->regenerate
      && (current_initramfs_args == NULL || !*current_initramfs_args)
      && (self->args == NULL || !*self->args))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "initramfs regeneration state is already %s",
                   current_regenerate ? "enabled" : "disabled");
      return FALSE;
    }

  rpmostree_origin_set_regenerate_initramfs (origin, self->regenerate, self->args);
  rpmostree_sysroot_upgrader_set_origin (upgrader, origin);

  if (!rpmostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
    return FALSE;

  if (self->reboot)
    rpmostreed_reboot (cancellable, error);

  return TRUE;
}

static void
initramfs_state_transaction_class_init (InitramfsStateTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = initramfs_state_transaction_finalize;

  class->execute = initramfs_state_transaction_execute;
}

static void
initramfs_state_transaction_init (InitramfsStateTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_initramfs_state (GDBusMethodInvocation *invocation,
                                            OstreeSysroot *sysroot,
                                            const char *osname,
                                            gboolean regenerate,
                                            char **args,
                                            gboolean reboot,
                                            GCancellable *cancellable,
                                            GError **error)
{
  InitramfsStateTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);

  self = g_initable_new (initramfs_state_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->regenerate = regenerate;
      self->args = g_strdupv (args);
      self->reboot = reboot;
    }

  return (RpmostreedTransaction *) self;
}

/* ================================ Cleanup ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  RpmOstreeTransactionCleanupFlags flags;
} CleanupTransaction;

typedef RpmostreedTransactionClass CleanupTransactionClass;

GType cleanup_transaction_get_type (void);

G_DEFINE_TYPE (CleanupTransaction,
               cleanup_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
cleanup_transaction_finalize (GObject *object)
{
  CleanupTransaction *self;

  self = (CleanupTransaction *) object;
  g_free (self->osname);

  G_OBJECT_CLASS (cleanup_transaction_parent_class)->finalize (object);
}

static gboolean
remove_directory_content_if_exists (int dfd,
                                    const char *path,
                                    GCancellable *cancellable,
                                    GError **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  glnx_autofd int fd = glnx_opendirat_with_errno (dfd, path, TRUE);
  if (fd < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "opendir(%s)", path);
    }
  else
    {
      if (!glnx_dirfd_iterator_init_take_fd (&fd, &dfd_iter, error))
        return FALSE;

      while (TRUE)
        {
          struct dirent *dent = NULL;

          if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
            return FALSE;
          if (dent == NULL)
            break;

          if (!glnx_shutil_rm_rf_at (dfd_iter.fd, dent->d_name, cancellable, error))
            return FALSE;
        }
    }
  return TRUE;
}

static gboolean
cleanup_transaction_execute (RpmostreedTransaction *transaction,
                             GCancellable *cancellable,
                             GError **error)
{
  CleanupTransaction *self = (CleanupTransaction *) transaction;
  const gboolean cleanup_pending = (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_PENDING_DEPLOY) > 0;
  const gboolean cleanup_rollback = (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_ROLLBACK_DEPLOY) > 0;

  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);
  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;

  if (cleanup_pending || cleanup_rollback)
    {
      g_autoptr(GPtrArray) new_deployments =
        rpmostree_syscore_filter_deployments (sysroot, self->osname,
                                              cleanup_pending,
                                              cleanup_rollback);

      if (new_deployments)
        {
          OstreeSysrootWriteDeploymentsOpts write_opts = { .do_postclean = FALSE };

          if (!ostree_sysroot_write_deployments_with_options (sysroot, new_deployments,
                                                              &write_opts, cancellable, error))
            return FALSE;

          /* And ensure we fall through to base cleanup */
          self->flags |= RPMOSTREE_TRANSACTION_CLEANUP_BASE;
        }
      else
        {
          rpmostree_output_message ("Deployments unchanged.");
        }
    }
  if (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_BASE)
    {
      if (!rpmostree_syscore_cleanup (sysroot, repo, cancellable, error))
        return FALSE;
    }
  if (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_REPOMD)
    {
      if (!remove_directory_content_if_exists (AT_FDCWD, RPMOSTREE_CORE_CACHEDIR, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static void
cleanup_transaction_class_init (CleanupTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = cleanup_transaction_finalize;

  class->execute = cleanup_transaction_execute;
}

static void
cleanup_transaction_init (CleanupTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_cleanup (GDBusMethodInvocation *invocation,
                                    OstreeSysroot         *sysroot,
                                    const char            *osname,
                                    RpmOstreeTransactionCleanupFlags flags,
                                    GCancellable          *cancellable,
                                    GError               **error)
{
  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);

  CleanupTransaction *self =
    g_initable_new (cleanup_transaction_get_type (),
                    cancellable, error,
                    "invocation", invocation,
                    "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                    NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->flags = flags;
    }

  return (RpmostreedTransaction *) self;

}

/* ================================ RefreshMd ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  RpmOstreeTransactionRefreshMdFlags flags;
} RefreshMdTransaction;

typedef RpmostreedTransactionClass RefreshMdTransactionClass;

GType refresh_md_transaction_get_type (void);

G_DEFINE_TYPE (RefreshMdTransaction,
               refresh_md_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
refresh_md_transaction_finalize (GObject *object)
{
  RefreshMdTransaction *self;

  self = (RefreshMdTransaction *) object;
  g_free (self->osname);

  G_OBJECT_CLASS (refresh_md_transaction_parent_class)->finalize (object);
}

static gboolean
refresh_md_transaction_execute (RpmostreedTransaction *transaction,
                                GCancellable *cancellable,
                                GError **error)
{
  RefreshMdTransaction *self = (RefreshMdTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);

  g_autoptr(OstreeDeployment) cfg_merge_deployment =
    ostree_sysroot_get_merge_deployment (sysroot, self->osname);
  g_autoptr(OstreeDeployment) origin_merge_deployment =
    rpmostree_syscore_get_origin_merge_deployment (sysroot, self->osname);

  /* but set the source root to be the origin merge deployment's so we pick up releasever */
  const char *sysroot_path = gs_file_get_path_cached (ostree_sysroot_get_path (sysroot));
  g_autofree char *origin_deployment_dirpath =
    ostree_sysroot_get_deployment_dirpath (sysroot, origin_merge_deployment);
  g_autofree char *origin_deployment_root =
    g_build_filename (sysroot_path, origin_deployment_dirpath, NULL);

  OstreeRepo *repo = ostree_sysroot_repo (sysroot);
  g_autoptr(RpmOstreeContext) ctx = rpmostree_context_new_system (repo, cancellable, error);

  /* We could bypass rpmostree_context_setup() here and call dnf_context_setup() ourselves
   * since we're not actually going to perform any installation. Though it does provide us
   * with the right semantics for install/source_root. So let's just play the game and
   * provide a dummy treespec. */
  if (!rpmostree_context_setup (ctx, NULL, origin_deployment_root, NULL, cancellable, error))
    return FALSE;

  if (self->flags & RPMOSTREE_TRANSACTION_REFRESH_MD_FLAG_FORCE)
    {
      DnfContext *dnfctx = rpmostree_context_get_dnf (ctx);
      dnf_context_set_cache_age (dnfctx, 0);
    }

  /* point libdnf to our repos dir */
  rpmostree_context_configure_from_deployment (ctx, sysroot, cfg_merge_deployment);

  if (!rpmostree_context_download_metadata (ctx, cancellable, error))
    return FALSE;

  return TRUE;
}

static void
refresh_md_transaction_class_init (CleanupTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = refresh_md_transaction_finalize;

  class->execute = refresh_md_transaction_execute;
}

static void
refresh_md_transaction_init (RefreshMdTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_refresh_md (GDBusMethodInvocation *invocation,
                                       OstreeSysroot         *sysroot,
                                       RpmOstreeTransactionRefreshMdFlags flags,
                                       const char            *osname,
                                       GCancellable          *cancellable,
                                       GError               **error)
{
  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);

  RefreshMdTransaction *self =
    g_initable_new (refresh_md_transaction_get_type (),
                    cancellable, error,
                    "invocation", invocation,
                    "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                    NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->flags = flags;
    }

  return (RpmostreedTransaction *) self;

}

/* ================================KernelArg================================ */

typedef struct {
  RpmostreedTransaction parent;
  char  *osname;
  char  *existing_kernel_args;
  char **kernel_args_added;
  char **kernel_args_deleted;
  char **kernel_args_replaced;
  RpmOstreeTransactionKernelArgFlags flags;
} KernelArgTransaction;

typedef RpmostreedTransactionClass KernelArgTransactionClass;

GType kernel_arg_transaction_get_type (void);

G_DEFINE_TYPE (KernelArgTransaction,
               kernel_arg_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
kernel_arg_transaction_finalize (GObject *object)
{
  KernelArgTransaction *self;

  self = (KernelArgTransaction *) object;
  g_free (self->osname);
  g_strfreev (self->kernel_args_added);
  g_strfreev (self->kernel_args_deleted);
  g_strfreev (self->kernel_args_replaced);
  g_free (self->existing_kernel_args);
  G_OBJECT_CLASS (kernel_arg_transaction_parent_class)->finalize (object);
}

static gboolean
kernel_arg_transaction_execute (RpmostreedTransaction *transaction,
                                GCancellable *cancellable,
                                GError **error)
{
  KernelArgTransaction *self = (KernelArgTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);

  /* Read in the existing kernel args and convert those to an #OstreeKernelArg instance for API usage */
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kargs = _ostree_kernel_args_from_string (self->existing_kernel_args);
  g_autoptr(RpmOstreeSysrootUpgrader) upgrader = rpmostree_sysroot_upgrader_new (sysroot, self->osname, 0,
                                                                                 cancellable, error);

  /* We need the upgrader to perform the deployment */
  if (upgrader == NULL)
    return FALSE;

  if (self->kernel_args_deleted)
    {
      /* Delete all the entries included in the kernel args */
      for (char **iter = self->kernel_args_deleted; iter && *iter; iter++)
        {
          const char*  arg =  *iter;
          if (!_ostree_kernel_args_delete (kargs, arg, error))
            return FALSE;
        }
    }
  else
    {
      if (self->kernel_args_replaced)
        {
          for (char **iter = self->kernel_args_replaced; iter && *iter; iter++)
            {
              const char *arg = *iter;
              if (!_ostree_kernel_args_new_replace (kargs, arg, error))
                return FALSE;
            }
        }

      if (self->kernel_args_added)
        _ostree_kernel_args_append_argv (kargs, self->kernel_args_added);
    }

  /* After all the arguments are processed earlier, we convert it to a string list*/
  g_auto(GStrv) kargs_strv = _ostree_kernel_args_to_strv (kargs);
  if (!rpmostree_sysroot_upgrader_deploy_set_kargs (upgrader, kargs_strv,
                                                    cancellable, error))
    return FALSE;
  if (self->flags & RPMOSTREE_TRANSACTION_KERNEL_ARG_FLAG_REBOOT)
    rpmostreed_reboot (cancellable, error);

  return TRUE;
}

static void
kernel_arg_transaction_class_init (KernelArgTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = kernel_arg_transaction_finalize;

  class->execute = kernel_arg_transaction_execute;
}

static void
kernel_arg_transaction_init (KernelArgTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_kernel_arg (GDBusMethodInvocation *invocation,
                                       OstreeSysroot         *sysroot,
                                       const char            *osname,
                                       const char            *existing_kernel_args,
                                       const char * const *kernel_args_added,
                                       const char * const *kernel_args_replaced,
                                       const char * const *kernel_args_deleted,
                                       RpmOstreeTransactionKernelArgFlags flags,
                                       GCancellable          *cancellable,
                                       GError               **error)
{
  KernelArgTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);

  self = g_initable_new (kernel_arg_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->kernel_args_added = strdupv_canonicalize (kernel_args_added);
      self->kernel_args_replaced = strdupv_canonicalize (kernel_args_replaced);
      self->kernel_args_deleted = strdupv_canonicalize (kernel_args_deleted);
      self->existing_kernel_args = g_strdup (existing_kernel_args);
      self->flags = flags;
    }

  return (RpmostreedTransaction *) self;
}
