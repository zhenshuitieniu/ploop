#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlwriter.h>
#include <errno.h>

#include "ploop.h"

static xmlNodePtr find_child_node(xmlNode *cur_node, const char *elem)
{
	xmlNodePtr child;

	for (child = cur_node->xmlChildrenNode; child != NULL; child = child->next) {
		if (!xmlStrcmp(child->name, (const xmlChar *) elem) &&
				child->type == XML_ELEMENT_NODE)
		{
			return child;
		}
	}
	return NULL;
}

static xmlNodePtr seek(xmlNodePtr root, const char *elem)
{
	xmlNodePtr childNode = root;
	const char *path, *p;
	char nodename[128];
	int len;
	int last = 0;

	path = elem;
	if (path[0] == '/')
		path++;
	if (path[0] == 0)
		return NULL;
	len = strlen(elem);
	while (!last) {
		if ((p = strchr(path, '/')) == NULL) {
			p = path + strlen(path);
			last = 1;
		}
		snprintf(nodename, p - path + 1, "%s", path);
		childNode = find_child_node(childNode, nodename);
		if (childNode == NULL)
			return NULL;
		path = ++p;
	}
	return childNode;
}

static const char *get_element_txt(xmlNode *node)
{
	xmlNode *child;

	for (child = node->xmlChildrenNode; child; child = child->next) {
		if (child->type == XML_TEXT_NODE)
			return (const char*)child->content;
	}
	return NULL;
}

static int parse_ul(const char *str, __u64 *val)
{
	char *endptr;

	if (str == NULL)
		return -1;

	*val = strtoul(str, &endptr, 0);
	if (endptr != NULL && *endptr != '\0')
		return -1;
	return 0;
}

#define ERR(var, name)							\
	do {								\
		if (var == NULL) {					\
			ploop_err(0, "Invalid disk descriptor file "	\
				"format: '" name "' node not found");	\
			return -1;					\
		}							\
	} while (0)

static int parse_xml(const char *basedir, xmlNode *root_node, struct ploop_disk_images_data *di)
{
	xmlNode *cur_node, *node;
	char image[MAXPATHLEN];
	const char *data = NULL;
	const char *file = NULL;
	const char *guid = NULL;
	const char *parent_guid = NULL;
	__u64 val;

	cur_node = seek(root_node, "/Disk_Parameters");
	ERR(cur_node, "/Disk_Parameters");

	node = seek(cur_node, "Disk_size");
	if (node != NULL) {
		data = get_element_txt(node);
		if (parse_ul(data, &val) == 0)
			di->size = val;
	}

	node = seek(cur_node, "Cylinders");
	if (node != NULL) {
		data = get_element_txt(node);
		if (parse_ul(data, &val) == 0)
			di->cylinders = (unsigned)val;
	}

	node = seek(cur_node, "Heads");
	if (node != NULL) {
		data = get_element_txt(node);
		if (parse_ul(data, &val) == 0)
			di->heads = (unsigned)val;
	}
	node = seek(cur_node, "Sectors");
	if (node != NULL) {
		data = get_element_txt(node);
		if (parse_ul(data, &val) == 0)
			di->sectors= (unsigned)val;
	}

	cur_node = seek(root_node, "/StorageData/Storage/Image");
	ERR(cur_node, "/StorageData/Storage/Image");

	for (; cur_node; cur_node = cur_node->next) {
		if (cur_node->type != XML_ELEMENT_NODE)
			continue;
		guid = NULL;
		node = seek(cur_node, "GUID");
		if (node != NULL) {
			guid = get_element_txt(node);
			if (guid == NULL)
				guid = "";
		}
		ERR(guid, "GUID");
		node = seek(cur_node, "Type");
		if (node != NULL) {
			data = get_element_txt(node);
			if (data != NULL && !strcmp(data, "Plain"))
				di->raw = 1;
		}
		file = NULL;
		node = seek(cur_node, "File");
		if (node != NULL) {
			file = get_element_txt(node);
			if (file != NULL) {
				if (basedir[0] != 0 && file[0] != '/')
					snprintf(image, sizeof(image), "%s/%s", basedir, file);
				else
					snprintf(image, sizeof(image), "%s", file);
			}
		}
		ERR(file, "File");

		if (ploop_add_image_entry(di, image, guid))
			return -1;
	}
	cur_node = seek(root_node, "/SnapshotTree");
	ERR(cur_node, "/SnapshotTree");

	node = seek(cur_node, "TopGUID");
	if (node != NULL) {
		data = get_element_txt(node);
		ERR(data, "TopGUID");
		di->top_guid = strdup(data);
	}

	cur_node = seek(root_node, "/SnapshotTree/Shot");
	if (cur_node != NULL) {
		for (; cur_node; cur_node = cur_node->next) {
			if (cur_node->type != XML_ELEMENT_NODE)
				continue;

			guid = NULL;
			node = seek(cur_node, "GUID");
			if (node != NULL)
				guid = get_element_txt(node);
			ERR(guid, "Snapshots GUID");

			parent_guid = NULL;
			node = seek(cur_node, "ParentGUID");
			if (node != NULL)
				parent_guid = get_element_txt(node);

			ERR(parent_guid, "ParentGUID");

			if (ploop_add_snapshot_entry(di, guid, parent_guid))
				return -1;
		}
	}
	return 0;
}
#undef ERR

void get_basedir(const char *fname, char *out, int len)
{
	char *p;

	strncpy(out, fname, len);

	p = strrchr(out, '/');
	if (p != NULL)
		*p = 0;
}

static int validate_disk_descriptor(struct ploop_disk_images_data *di)
{
	if (di->nimages == 0) {
		ploop_err(0, "No images found in %s",
				di->runtime->xml_fname);
		return -1;
	}
	// FIXME: compatibility issue have to be removed before BETA
	if (di->nimages != di->nsnapshots) {
		int ret;

		ret = ploop_add_snapshot_entry(di, BASE_UUID, NONE_UUID);
		if (ret)
			return ret;
		if (di->top_guid == NULL)
			di->top_guid = strdup(BASE_UUID);
	}
	if (!is_valid_guid(di->top_guid)) {
		ploop_err(0, "Validation of %s failed: invalid top delta %s",
				di->runtime->xml_fname, di->top_guid);
		return -1;
	}
	if (di->nimages != di->nsnapshots) {
		ploop_err(0, "Validation of %s failed: images(%d) != snapshots(%d)",
				di->runtime->xml_fname, di->nimages, di->nsnapshots);
		return -1;
	}
	return 0;
}

int ploop_read_diskdescriptor(const char *fname, struct ploop_disk_images_data *di)
{
	int ret;
	char path[MAXPATHLEN];
	char basedir[MAXPATHLEN];
	xmlDoc *doc = NULL;
	xmlNode *root_element = NULL;

	LIBXML_TEST_VERSION

	if (di == NULL)
		return -1;

	if (realpath(fname, path) == NULL) {
		ploop_err(errno, "Can't resolve %s", fname);
		return -1;
	}

	di->runtime->xml_fname = strdup(path);

	doc = xmlReadFile(fname, NULL, 0);
	if (doc == NULL) {
		ploop_err(0, "Can't parse %s", fname);
		return -1;
	}
	root_element = xmlDocGetRootElement(doc);

	get_basedir(path, basedir, sizeof(path));
	ret = parse_xml(basedir, root_element, di);
	if (ret == 0)
		ret = validate_disk_descriptor(di);

	xmlFreeDoc(doc);
	xmlCleanupParser();

	return ret;
}

static int normalize_image_name(const char *basedir, const char *image, char *out, int len)
{
	const char *p;
	int n;

	n = strlen(basedir);
	p = image;
	if (strncmp(image, basedir, n) == 0)
		p += n + 1;

	snprintf(out, len, "%s", p);

	return 0;
}

int ploop_store_diskdescriptor(const char *fname, struct ploop_disk_images_data *di)
{
	int i, rc = -1;
	xmlTextWriterPtr writer = NULL;
	xmlDocPtr doc = NULL;
	char tmp[MAXPATHLEN];
	char basedir[MAXPATHLEN];

	ploop_log(0, "Storing %s", fname);

	get_basedir(fname, tmp, sizeof(tmp));

	if (realpath(tmp, basedir) == NULL) {
		ploop_err(errno, "Can't resolve %s", tmp);
		return -1;
	}

	doc = xmlNewDoc(BAD_CAST XML_DEFAULT_VERSION);
	if (doc == NULL) {
		ploop_err(0, "Error creating xml document tree");
		return -1;
	}

	/* Create a new XmlWriter for DOM tree, with no compression. */
	writer = xmlNewTextWriterTree(doc, NULL, 0);
	if (writer == NULL) {
		ploop_err(0, "Error creating xml writer");
		goto err;
	}

	/* Start the document with the xml default for the version,
	 * encoding ISO 8859-1 and the default for the standalone
	 * declaration. */
	rc = xmlTextWriterStartDocument(writer, NULL, NULL, NULL);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriterStartDocument");
		goto err;
	}
	rc = xmlTextWriterStartElement(writer, BAD_CAST "Parallels_disk_image");
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriterStartDocument");
		goto err;
	}
	/*********************************************
	 *	Disk_Parameters
	 ********************************************/
	rc = xmlTextWriterStartElement(writer, BAD_CAST "Disk_Parameters");
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter Disk_Parameters");
		goto err;
	}
	rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST "Disk_size", "%llu", di->size);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter Disk_size");
		goto err;
	}

	rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST "Cylinders", "%u", di->cylinders);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter Cylinders");
		goto err;
	}
	rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST "Heads", "%u", di->heads);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter Heads");
		goto err;
	}
	rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST "Sectors", "%llu",
			di->size /(di->cylinders * di->heads));
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter Sectors");
		goto err;
	}
	rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST "Padding", "%u", 0);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter Padding");
		goto err;
	}

	/* Close   Disk_Parameters */
	rc = xmlTextWriterEndElement(writer);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriterEndElement");
		goto err;
	}
	/****************************************
	 * StorageData
	 ****************************************/
	rc = xmlTextWriterStartElement(writer, BAD_CAST "StorageData");
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter StorageData");
		goto err;
	}
	/* Start an element named "Storage" as child of StorageData. */
	rc = xmlTextWriterStartElement(writer, BAD_CAST "Storage");
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter Storage");
		goto err;
	}
	rc = xmlTextWriterWriteElement(writer, BAD_CAST "Start",  BAD_CAST "0");
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter Start");
		goto err;
	}
	rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST "End", "%llu", di->size);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter End");
		goto err;
	}
	rc = xmlTextWriterWriteElement(writer, BAD_CAST "Blocksize", BAD_CAST "512");
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter Blocksize");
		goto err;
	}
	/****************************************
	 *	Images
	 ****************************************/
	for (i = 0; i < di->nimages; i++) {
		rc = xmlTextWriterStartElement(writer, BAD_CAST "Image");
		if (rc < 0) {
			ploop_err(0, "Error at xmlTextWriter Image");
			goto err;
		}

		rc = xmlTextWriterWriteElement(writer, BAD_CAST "GUID",
				BAD_CAST di->images[i]->guid);
		if (rc < 0) {
			ploop_err(0, "Error at xmlTextWriter GUID");
			goto err;
		}
		rc = xmlTextWriterWriteElement(writer, BAD_CAST "Type",
			BAD_CAST (di->raw ? "Plain" : "Compressed"));
		if (rc < 0) {
			ploop_err(0, "Error at xmlTextWriter Type");
			goto err;
		}

		normalize_image_name(basedir, di->images[i]->file, tmp, sizeof(tmp));
		rc = xmlTextWriterWriteElement(writer, BAD_CAST "File",
				BAD_CAST tmp);
		if (rc < 0) {
			ploop_err(0, "Error at xmlTextWriter File");
			goto err;
		}

		/*  Close  Image */
		rc = xmlTextWriterEndElement(writer);
		if (rc < 0) {
			ploop_err(0, "Error at xmlTextWriterEndElement");
			goto err;
		}
	}

	/* Close Storage */
	rc = xmlTextWriterEndElement(writer);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriterEndElement");
		goto err;
	}
	/* Close StorageData. */
	rc = xmlTextWriterEndElement(writer);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriterEndElement");
		goto err;
	}
	/****************************************
	  Compatibility entry. handle validation in PrlDiskDescriptor::OpenDescriptor()
		<Shot>
		 <GUID>{5fbaabe3-6958-40ff-92a7-860e329aab41}</GUID>
		 <ParentGUID>{00000000-0000-0000-0000-000000000000}</ParentGUID>
		</Shot>
	 ****************************************/
	rc = xmlTextWriterStartElement(writer, BAD_CAST "Snapshots");
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter Snapshots");
		goto err;
	}
	rc = xmlTextWriterStartElement(writer, BAD_CAST "Shot");
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter Shot");
		goto err;
	}

	rc = xmlTextWriterWriteElement(writer, BAD_CAST "GUID",
			BAD_CAST BASE_UUID);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter GUID");
		goto err;
	}
	rc = xmlTextWriterWriteElement(writer, BAD_CAST "ParentGUID",
			BAD_CAST NONE_UUID);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter ParentGUID");
		goto err;
	}
	/*  Close Shot */
	rc = xmlTextWriterEndElement(writer);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriterEndElement");
		goto err;
	}
	/*  Close Snapshots */
	rc = xmlTextWriterEndElement(writer);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriterEndElement");
		goto err;
	}

	/****************************************
	 *	Snapshot
	 ****************************************/
	rc = xmlTextWriterStartElement(writer, BAD_CAST "SnapshotTree");
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriter SnapshotTree");
		goto err;
	}
	if (di->top_guid != NULL) {
		rc = xmlTextWriterWriteElement(writer, BAD_CAST "TopGUID",
				BAD_CAST di->top_guid);
		if (rc < 0) {
			ploop_err(0, "Error at xmlTextWriter TopGUID");
			goto err;
		}
	}

	/****************************************
	 *      Shot
	 ****************************************/
	for (i = 0; i < di->nsnapshots; i++) {
		rc = xmlTextWriterStartElement(writer, BAD_CAST "Shot");
		if (rc < 0) {
			ploop_err(0, "Error at xmlTextWriter Shot");
			goto err;
		}

		rc = xmlTextWriterWriteElement(writer, BAD_CAST "GUID",
				BAD_CAST di->snapshots[i]->guid);
		if (rc < 0) {
			ploop_err(0, "Error at xmlTextWriterWrite GUID");
			goto err;
		}
		rc = xmlTextWriterWriteElement(writer, BAD_CAST "ParentGUID",
				BAD_CAST di->snapshots[i]->parent_guid);
		if (rc < 0) {
			ploop_err(0, "Error at xmlTextWriter ParentGUID");
			goto err;
		}

		/*  Close Shot */
		rc = xmlTextWriterEndElement(writer);
		if (rc < 0) {
			ploop_err(0, "Error at xmlTextWriterEndElement");
			goto err;
		}
	}
	/* Close SnapshotTree */
	rc = xmlTextWriterEndElement(writer);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriterEndElement");
		goto err;
	}

	/* Close Parallels_disk_image */
	rc = xmlTextWriterEndElement(writer);
	if (rc < 0) {
		ploop_err(0, "Error at xmlTextWriterEndElement");
		goto err;
	}
	xmlFreeTextWriter(writer);
	writer = NULL;
	snprintf(tmp, sizeof(tmp), "%s.tmp", fname);
	rc = xmlSaveFormatFile(tmp, doc, 1);
	if (rc < 0) {
		ploop_err(0, "Error at xmlSaveFormatFile %s", tmp);
		goto err;
	}
	rename(tmp, fname);
	rc = 0;
err:
	if (writer)
		xmlFreeTextWriter(writer);
	if (doc)
		xmlFreeDoc(doc);

	return rc;
}
