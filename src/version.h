/*
 * Copyright (C) 2005-2010 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or 
 * (at your option) any later version.
 *
 * See files COPYING.* for License information.
 */

/**
 * @brief this file contains the version of hamster
 *
 */

#ifndef HAM_VERSION_H__
#define HAM_VERSION_H__

#ifdef __cplusplus
extern "C" {
#endif 

/**
 * the version numbers
 *
 * @remark a change of the major revision means a significant update
 * with a lot of new features, API changes and an incompatible database
 * format.
 * The minor revision means that updates were not so significant, but
 * the database format is no longer compatible. a change of the revision
 * means the release is a bugfix with a compatible database format.
 */
#define HAM_VERSION_MAJ 1
#define HAM_VERSION_MIN 1
#define HAM_VERSION_REV 4
#define HAM_VERSION_STR "1.1.4"


#ifdef __cplusplus
} // extern "C"
#endif 

#endif /* HAM_VERSION_H__ */
