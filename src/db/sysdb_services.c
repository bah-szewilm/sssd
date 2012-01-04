/*
    SSSD

    Authors:
        Stephen Gallagher <sgallagh@redhat.com>

    Copyright (C) 2012 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "util/util.h"
#include "db/sysdb.h"
#include "db/sysdb_private.h"
#include "db/sysdb_services.h"

errno_t
sysdb_svc_add(TALLOC_CTX *mem_ctx,
              struct sysdb_ctx *sysdb,
              const char *primary_name,
              int port,
              const char **aliases,
              const char **protocols,
              struct ldb_dn **dn);

static errno_t
sysdb_svc_update(struct sysdb_ctx *sysdb,
                 struct ldb_dn *dn,
                 int port,
                 const char **aliases,
                 const char **protocols);

errno_t
sysdb_svc_remove_alias(struct sysdb_ctx *sysdb,
                       struct ldb_dn *dn,
                       const char *alias);

errno_t
sysdb_getservbyname(TALLOC_CTX *mem_ctx,
                    struct sysdb_ctx *sysdb,
                    const char *name,
                    const char *proto,
                    struct ldb_result **_res)
{
    errno_t ret;
    int lret;
    TALLOC_CTX *tmp_ctx;
    static const char *attrs[] = SYSDB_SVC_ATTRS;
    struct ldb_dn *base_dn;
    struct ldb_result *res;
    char *sanitized_name;
    char *sanitized_proto;

    *_res = NULL;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        return ENOMEM;
    }

    base_dn = ldb_dn_new_fmt(tmp_ctx, sysdb->ldb,
                             SYSDB_TMPL_SVC_BASE,
                             sysdb->domain->name);
    if (!base_dn) {
        ret = ENOMEM;
        goto done;
    }

    ret = sss_filter_sanitize(tmp_ctx, name, &sanitized_name);
    if (ret != EOK) {
        goto done;
    }

    if (proto) {
        ret = sss_filter_sanitize(tmp_ctx, proto, &sanitized_proto);
        if (ret != EOK) {
            goto done;
        }
    }

    lret = ldb_search(sysdb->ldb, tmp_ctx, &res, base_dn,
                      LDB_SCOPE_SUBTREE, attrs,
                      SYSDB_SVC_BYNAME_FILTER,
                      proto?sanitized_proto:"*",
                      sanitized_name, sanitized_name);
    if (lret != LDB_SUCCESS) {
        ret = sysdb_error_to_errno(lret);
        goto done;
    }

    if (res->count == 0) {
        ret = ENOENT;
        goto done;
    }

    *_res = talloc_steal(mem_ctx, res);

    ret = EOK;

done:
    talloc_free(tmp_ctx);
    return ret;
}

errno_t
sysdb_getservbyport(TALLOC_CTX *mem_ctx,
                    struct sysdb_ctx *sysdb,
                    int port,
                    const char *proto,
                    struct ldb_result **_res)
{
    errno_t ret;
    int lret;
    TALLOC_CTX *tmp_ctx;
    static const char *attrs[] = SYSDB_SVC_ATTRS;
    struct ldb_dn *base_dn;
    struct ldb_result *res;
    char *sanitized_proto = NULL;

    if (port <= 0) {
        return EINVAL;
    }

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        return ENOMEM;
    }

    base_dn = ldb_dn_new_fmt(tmp_ctx, sysdb->ldb,
                             SYSDB_TMPL_SVC_BASE,
                             sysdb->domain->name);
    if (!base_dn) {
        ret = ENOMEM;
        goto done;
    }

    if (proto) {
        ret = sss_filter_sanitize(tmp_ctx, proto, &sanitized_proto);
        if (ret != EOK) {
            goto done;
        }
    }

    lret = ldb_search(sysdb->ldb, tmp_ctx, &res, base_dn,
                      LDB_SCOPE_SUBTREE, attrs,
                      SYSDB_SVC_BYPORT_FILTER,
                      sanitized_proto?sanitized_proto:"*",
                      (unsigned int) port);
    if (lret) {
        ret = sysdb_error_to_errno(lret);
        goto done;
    }

    if (res->count == 0) {
        ret = ENOENT;
        goto done;
    }

    *_res = talloc_steal(mem_ctx, res);

    ret = EOK;

done:
    talloc_free(tmp_ctx);
    return ret;
}

errno_t
sysdb_store_service(struct sysdb_ctx *sysdb,
                    const char *primary_name,
                    int port,
                    const char **aliases,
                    const char **protocols,
                    uint64_t cache_timeout,
                    time_t now)
{
    errno_t ret;
    errno_t sret;
    TALLOC_CTX *tmp_ctx;
    bool in_transaction = false;
    struct ldb_result *res = NULL;
    const char *name;
    unsigned int i;
    struct ldb_dn *update_dn = NULL;
    struct sysdb_attrs *attrs;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return ENOMEM;

    ret = sysdb_transaction_start(sysdb);
    if (ret != EOK) goto done;

    in_transaction = true;

    /* Check that the port is unique
     * If the port appears for any service other than
     * the one matching the primary_name, we need to
     * remove them so that getservbyport() can work
     * properly. Last entry saved to the cache should
     * always "win".
     */
    ret = sysdb_getservbyport(tmp_ctx, sysdb, port, NULL, &res);
    if (ret != EOK && ret != ENOENT) {
        goto done;
    } else if (ret != ENOENT) {
        if (res->count != 1) {
            /* Somehow the cache has multiple entries with
             * the same port. This is corrupted. We'll delete
             * them all to sort it out.
             */
            for (i = 0; i < res->count; i++) {
                DEBUG(SSSDBG_TRACE_FUNC,
                      ("Corrupt cache entry [%s] detected. Deleting\n",
                       ldb_dn_canonical_string(tmp_ctx,
                                               res->msgs[i]->dn)));

                ret = sysdb_delete_entry(sysdb, res->msgs[i]->dn, true);
                if (ret != EOK) {
                    DEBUG(SSSDBG_MINOR_FAILURE,
                          ("Could not delete corrupt cache entry [%s]\n",
                           ldb_dn_canonical_string(tmp_ctx,
                                                   res->msgs[i]->dn)));
                    goto done;
                }
            }
        } else {
            /* Check whether this is the same name as we're currently
             * saving to the cache.
             */
            name = ldb_msg_find_attr_as_string(res->msgs[0],
                                               SYSDB_NAME,
                                               NULL);
            if (!name || strcmp(name, primary_name) != 0) {

                if (!name) {
                    DEBUG(SSSDBG_CRIT_FAILURE,
                          ("A service with no name?\n"));
                    /* Corrupted */
                }

                /* Either this is a corrupt entry or it's another service
                 * claiming ownership of this port. In order to account
                 * for port reassignments, we need to delete the old entry.
                 */
                DEBUG(SSSDBG_TRACE_FUNC,
                      ("Corrupt or replaced cache entry [%s] detected. "
                       "Deleting\n",
                       ldb_dn_canonical_string(tmp_ctx,
                                               res->msgs[0]->dn)));

                ret = sysdb_delete_entry(sysdb, res->msgs[0]->dn, true);
                if (ret != EOK) {
                    DEBUG(SSSDBG_MINOR_FAILURE,
                          ("Could not delete cache entry [%s]\n",
                           ldb_dn_canonical_string(tmp_ctx,
                                                   res->msgs[0]->dn)));
                }
            }
        }
    }
    talloc_zfree(res);

    /* Ok, ports should now be unique. Now look
     * the service up by name to determine if we
     * need to update existing entries or modify
     * aliases.
     */
    ret = sysdb_getservbyname(tmp_ctx, sysdb, primary_name, NULL, &res);
    if (ret != EOK && ret != ENOENT) {
        goto done;
    } else if (ret != ENOENT) { /* Found entries */
        for (i = 0; i < res->count; i++) {
            /* Check whether this is the same name as we're currently
             * saving to the cache.
             */
            name = ldb_msg_find_attr_as_string(res->msgs[i],
                                               SYSDB_NAME,
                                               NULL);
            if (!name) {

                /* Corrupted */
                DEBUG(SSSDBG_CRIT_FAILURE,
                      ("A service with no name?\n"));
                DEBUG(SSSDBG_TRACE_FUNC,
                      ("Corrupt cache entry [%s] detected. Deleting\n",
                       ldb_dn_canonical_string(tmp_ctx,
                                               res->msgs[i]->dn)));

                ret = sysdb_delete_entry(sysdb, res->msgs[i]->dn, true);
                if (ret != EOK) {
                    DEBUG(SSSDBG_MINOR_FAILURE,
                          ("Could not delete corrupt cache entry [%s]\n",
                           ldb_dn_canonical_string(tmp_ctx,
                                                   res->msgs[i]->dn)));
                    goto done;
                }
            } else if (strcmp(name, primary_name) == 0) {
                /* This is the same service name, so we need
                 * to update this entry with the values
                 * provided.
                 */
                if(update_dn) {
                    DEBUG(SSSDBG_CRIT_FAILURE,
                          ("Two existing services with the same name: [%s]? "
                           "Deleting both.\n",
                           primary_name));

                    /* Delete the entry from the previous pass */
                    ret = sysdb_delete_entry(sysdb, update_dn, true);
                    if (ret != EOK) {
                        DEBUG(SSSDBG_MINOR_FAILURE,
                              ("Could not delete cache entry [%s]\n",
                               ldb_dn_canonical_string(tmp_ctx,
                                                       update_dn)));
                        goto done;
                    }

                    /* Delete the new entry as well */
                    ret = sysdb_delete_entry(sysdb, res->msgs[i]->dn, true);
                    if (ret != EOK) {
                        DEBUG(SSSDBG_MINOR_FAILURE,
                              ("Could not delete cache entry [%s]\n",
                               ldb_dn_canonical_string(tmp_ctx,
                                                       res->msgs[i]->dn)));
                        goto done;
                    }

                    update_dn = NULL;
                } else {
                    update_dn = talloc_steal(tmp_ctx, res->msgs[i]->dn);
                }
            } else {
                /* Another service is claiming this name as an alias.
                 * In order to account for aliases being promoted to
                 * primary names, we need to make sure to remove the
                 * old alias entry.
                 */
                ret = sysdb_svc_remove_alias(sysdb,
                                             res->msgs[i]->dn,
                                             primary_name);
                if (ret != EOK) goto done;
            }
        }
        talloc_zfree(res);
    }

    if (update_dn) {
        /* Update the existing entry */
        ret = sysdb_svc_update(sysdb, update_dn, port, aliases, protocols);
    } else {
        /* Add a new entry */
        ret = sysdb_svc_add(tmp_ctx, sysdb, primary_name, port,
                            aliases, protocols, &update_dn);
    }
    if (ret != EOK) goto done;

    /* Set the cache timeout */
    attrs = sysdb_new_attrs(tmp_ctx);
    if (!attrs) {
        ret = ENOMEM;
        goto done;
    }
    ret = sysdb_attrs_add_time_t(attrs, SYSDB_LAST_UPDATE, now);
    if (ret) goto done;

    ret = sysdb_attrs_add_time_t(attrs, SYSDB_CACHE_EXPIRE,
                                 ((cache_timeout) ?
                                  (now + cache_timeout) : 0));
    if (ret) goto done;

    ret = sysdb_set_entry_attr(sysdb, update_dn, attrs, SYSDB_MOD_REP);
    if (ret != EOK) goto done;

    ret = sysdb_transaction_commit(sysdb);
    if (ret == EOK) in_transaction = false;

done:
    if (in_transaction) {
        sret = sysdb_transaction_cancel(sysdb);
        if (sret != EOK) {
            DEBUG(SSSDBG_CRIT_FAILURE, ("Could not cancel transaction\n"));
        }
    }
    talloc_free(tmp_ctx);
    return ret;
}

struct ldb_dn *
sysdb_svc_dn(struct sysdb_ctx *sysdb, TALLOC_CTX *mem_ctx,
             const char *domain, const char *name)
{
    errno_t ret;
    char *clean_name;
    struct ldb_dn *dn;

    ret = sysdb_dn_sanitize(NULL, name, &clean_name);
    if (ret != EOK) {
        return NULL;
    }

    dn = ldb_dn_new_fmt(mem_ctx, sysdb->ldb, SYSDB_TMPL_SVC,
                        clean_name, domain);
    talloc_free(clean_name);

    return dn;
}

errno_t
sysdb_svc_add(TALLOC_CTX *mem_ctx,
              struct sysdb_ctx *sysdb,
              const char *primary_name,
              int port,
              const char **aliases,
              const char **protocols,
              struct ldb_dn **dn)
{
    errno_t ret;
    int lret;
    TALLOC_CTX *tmp_ctx;
    struct ldb_message *msg;
    unsigned long i;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return ENOMEM;

    msg = ldb_msg_new(tmp_ctx);
    if (!msg) {
        ret = ENOMEM;
        goto done;
    }

    /* svc dn */
    msg->dn = sysdb_svc_dn(sysdb, msg, sysdb->domain->name, primary_name);
    if (!msg->dn) {
        ret = ENOMEM;
        goto done;
    }

    /* Objectclass */
    ret = add_string(msg, LDB_FLAG_MOD_ADD,
                     SYSDB_OBJECTCLASS, SYSDB_SVC_CLASS);
    if (ret != EOK) goto done;

    /* Set the primary name */
    ret = add_string(msg, LDB_FLAG_MOD_ADD,
                     SYSDB_NAME, primary_name);
    if (ret != EOK) goto done;

    /* Set the port number */
    ret = add_ulong(msg, LDB_FLAG_MOD_ADD,
                    SYSDB_SVC_PORT, port);
    if (ret != EOK) goto done;

    /* If this service has any aliases, include them */
    if (aliases && aliases[0]) {
        /* Set the name aliases */
        lret = ldb_msg_add_empty(msg, SYSDB_NAME_ALIAS,
                                 LDB_FLAG_MOD_ADD, NULL);
        if (lret != LDB_SUCCESS) {
            ret = sysdb_error_to_errno(lret);
            goto done;
        }
        for (i=0; aliases[i]; i++) {
            lret = ldb_msg_add_string(msg, SYSDB_NAME_ALIAS, aliases[i]);
            if (lret != LDB_SUCCESS) {
                ret = sysdb_error_to_errno(lret);
                goto done;
            }
        }
    }

    /* Set the protocols */
    lret = ldb_msg_add_empty(msg, SYSDB_SVC_PROTO,
                             LDB_FLAG_MOD_ADD, NULL);
    if (lret != LDB_SUCCESS) {
        ret = sysdb_error_to_errno(lret);
        goto done;
    }
    for (i=0; protocols[i]; i++) {
        lret = ldb_msg_add_string(msg, SYSDB_SVC_PROTO, protocols[i]);
        if (lret != LDB_SUCCESS) {
            ret = sysdb_error_to_errno(lret);
            goto done;
        }
    }

    /* creation time */
    ret = add_ulong(msg, LDB_FLAG_MOD_ADD, SYSDB_CREATE_TIME,
                    (unsigned long)time(NULL));
    if (ret) goto done;

    lret = ldb_add(sysdb->ldb, msg);
    ret = sysdb_error_to_errno(lret);

    if (ret == EOK && dn) {
        *dn = talloc_steal(mem_ctx, msg->dn);
    }

done:
    if (ret) {
        DEBUG(SSSDBG_TRACE_INTERNAL,
              ("Error: %d (%s)\n", ret, strerror(ret)));
    }
    talloc_free(tmp_ctx);
    return ret;
}

static errno_t
sysdb_svc_update(struct sysdb_ctx *sysdb,
                 struct ldb_dn *dn,
                 int port,
                 const char **aliases,
                 const char **protocols)
{
    errno_t ret;
    struct ldb_message *msg;
    int lret;
    unsigned int i;

    if (!dn || !protocols || !protocols[0]) {
        return EINVAL;
    }

    msg = ldb_msg_new(NULL);
    if (!msg) {
        ret = ENOMEM;
        goto done;
    }

    msg->dn = dn;

    /* Update the port */
    ret = add_ulong(msg, SYSDB_MOD_REP,
                    SYSDB_SVC_PORT, port);
    if (ret != EOK) goto done;

    if (aliases && aliases[0]) {
        /* Update the aliases */
        lret = ldb_msg_add_empty(msg, SYSDB_NAME_ALIAS, SYSDB_MOD_REP, NULL);
        if (lret != LDB_SUCCESS) {
            ret = ENOMEM;
            goto done;
        }

        for (i = 0; aliases[i]; i++) {
            lret = ldb_msg_add_fmt(msg, SYSDB_NAME_ALIAS, "%s", aliases[i]);
            if (lret != LDB_SUCCESS) {
                ret = EINVAL;
                goto done;
            }
        }
    }

    /* Update the protocols */
    lret = ldb_msg_add_empty(msg, SYSDB_SVC_PROTO, SYSDB_MOD_REP, NULL);
    if (lret != LDB_SUCCESS) {
        ret = ENOMEM;
        goto done;
    }

    for (i = 0; protocols[i]; i++) {
        lret = ldb_msg_add_fmt(msg, SYSDB_SVC_PROTO, "%s", protocols[i]);
        if (lret != LDB_SUCCESS) {
            ret = EINVAL;
            goto done;
        }
    }

    lret = ldb_modify(sysdb->ldb, msg);
    ret = sysdb_error_to_errno(lret);

done:
    if (ret) {
        DEBUG(SSSDBG_TRACE_INTERNAL,
              ("Error: %d (%s)\n", ret, strerror(ret)));
    }
    talloc_free(msg);
    return ret;
}

errno_t
sysdb_svc_remove_alias(struct sysdb_ctx *sysdb,
                       struct ldb_dn *dn,
                       const char *alias)
{
    errno_t ret;
    struct ldb_message *msg;
    int lret;

    msg = ldb_msg_new(NULL);
    if (!msg) {
        ret = ENOMEM;
        goto done;
    }

    msg->dn = dn;

    ret = add_string(msg, SYSDB_MOD_DEL,
                     SYSDB_NAME_ALIAS, alias);
    if (ret != EOK) goto done;

    lret = ldb_modify(sysdb->ldb, msg);
    ret = sysdb_error_to_errno(lret);

done:
    if (ret) {
        DEBUG(SSSDBG_TRACE_INTERNAL,
              ("Error: %d (%s)\n", ret, strerror(ret)));
    }
    talloc_zfree(msg);
    return ret;
}

errno_t
sysdb_svc_delete(struct sysdb_ctx *sysdb,
                 const char *name,
                 int port,
                 const char *proto)
{
    errno_t ret, sret;
    TALLOC_CTX *tmp_ctx;
    struct ldb_result *res;
    unsigned int i;
    bool in_transaction;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        return ENOMEM;
    }

    ret = sysdb_transaction_start(sysdb);
    if (ret != EOK) goto done;

    in_transaction = true;

    if (name) {
        ret = sysdb_getservbyname(tmp_ctx, sysdb, name, proto, &res);
        if (ret != EOK && ret != ENOENT) goto done;
        if (ret == ENOENT) {
            /* Doesn't exist in the DB. Nothing to do */
            ret = EOK;
            goto done;
        }
    } else {
        ret = sysdb_getservbyport(tmp_ctx, sysdb, port, proto, &res);
        if (ret != EOK && ret != ENOENT) goto done;
        if (ret == ENOENT) {
            /* Doesn't exist in the DB. Nothing to do */
            ret = EOK;
            goto done;
        }
    }

    /* There should only be one matching entry,
     * but if there are multiple, we should delete
     * them all to de-corrupt the DB.
     */
    for (i = 0; i < res->count; i++) {
        ret = sysdb_delete_entry(sysdb, res->msgs[i]->dn, false);
        if (ret != EOK) goto done;
    }

    ret = sysdb_transaction_commit(sysdb);
    in_transaction = false;

done:
    if (in_transaction) {
        sret = sysdb_transaction_cancel(sysdb);
        if (sret != EOK) {
            DEBUG(SSSDBG_CRIT_FAILURE,
                  ("Could not cancel transaction\n"));
        }
    }
    if (ret != EOK && ret != ENOENT) {
        DEBUG(SSSDBG_TRACE_INTERNAL,
              ("Error: %d (%s)\n", ret, strerror(ret)));
    }
    talloc_zfree(tmp_ctx);
    return ret;
}
