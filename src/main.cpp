
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <ghc/filesystem.hpp>
#include <span>
#include <Geode/utils/cocos.hpp>
#include <string_view>

using namespace geode;
using namespace cocos2d;
using namespace geode::log;
namespace fs = ghc::filesystem;
template<typename T> using Arr = geode::cocos::CCArrayExt<T>;
using GameObjArr = Arr<GameObject*>;

inline auto playlayer() {return GameManager::sharedState()->m_playLayer;}
inline auto editor() {return GameManager::sharedState()->m_levelEditorLayer;}

template<typename T>
inline T& from_offset(void* self, uintptr_t offset) {
	return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(self) + offset);
}

gd::unordered_map<int, int>& getRobtopsItemIdsMap(PlayLayer* pl = playlayer())
{
	return playlayer()->m_effectManager->m_itemIDs;
}

constexpr int INVALID_INT = -1;

bool isNumeric(std::string_view str)
{
	for(const auto& c : str)
		if(!std::isdigit(c))
			return false;
	return true;
}

std::optional<int> countForItem(int itemid, PlayLayer* pl = playlayer())
{
#if defined(GEODE_IS_WINDOWS)
	auto& gdmap = getRobtopsItemIdsMap();
	if(auto it = gdmap.find(itemid); it != gdmap.end()) return it->second;
	return {};
#else
	return pl->m_effectManager->countForItem(itemid);
#endif
}

void updateCountForItem(int id, int value, PlayLayer* pl = playlayer())
{
	pl->m_effectManager->updateCountForItem(id, value);
}


//@{key} {value}
std::optional<std::string_view> getValueFromText(std::string_view line, std::string_view key)
{
    constexpr auto npos = std::string_view::npos;
    if(line[0] != '@') return {}; //return early if not comment (avoid format)

    auto start = line.find(fmt::format("@{}", key).c_str());
    if (start == npos) return {};

    auto startValue = line.find_first_of(' ', start);
    if (startValue == npos) return {};

    return line.substr(startValue + 1, line.size() - startValue);
};

std::optional<int> getNumericValueFromText(std::string_view line, std::string_view key)
{
	return geode::utils::numFromString<int>(getValueFromText(line, key).value_or("")).ok();
}

std::optional<std::pair<int, std::string_view>> getValueFromTextNumericKey(std::string_view line)
{
    constexpr auto npos = std::string_view::npos;
    if(line[0] != '@') return {};

    auto startValue = line.find_first_of(' ');
    if (startValue == npos) return {};

    auto key = line.substr(1, startValue - 1);
    if(!isNumeric(key)) return {};

    auto value = line.substr(startValue + 1);

	if(auto key_i = geode::utils::numFromString<int>(key); key_i.isOk())
	{
	    decltype(getValueFromTextNumericKey(line)) ret;
		ret.emplace(key_i.unwrap(), value);
		return ret;
	}
	return {};
};

struct PersistentID
{
	std::string key;
	int itemID = 0;
	bool operator==(const PersistentID& b) const { return itemID == b.itemID && key == b.key; };


	static std::optional<PersistentID> fromLevelText(std::string_view line)
	{
		auto pair_opt = getValueFromTextNumericKey(line);
		if(!pair_opt) return {};
		return PersistentID{.key = std::string(pair_opt->second), .itemID = pair_opt->first};
	}
};

struct PersPickup
{
	struct Members
	{
		bool mod_enabled = false;
		bool hasLoadedOnce = false;
		int accID = INVALID_INT;
		int restoreGroup = INVALID_INT;
		int saveGroup = INVALID_INT;
		int applyGroup = INVALID_INT;
		int enabledGroup = INVALID_INT;

 		//key <-> item ID, NOT VALUE
		//this is loaded from text objects when the level starts
		std::vector<PersistentID> levelSpecifiedPersItemIds;
		std::unordered_map<int, int> temp_died;

	} m;

	std::optional<int> persistentItemIdForKey(std::string_view key)
	{
		for(const auto& persid : m.levelSpecifiedPersItemIds)
		{
			if(persid.key == key)
			{
				return persid.itemID;
			}
		}
		return {};
	}

	static std::optional<int> getAccIdFromLevel(GJGameLevel* level)
	{
		int id = [&](){
			if(level->m_levelType == GJLevelType::Editor) return GJAccountManager::sharedState()->m_accountID;
			else return level->m_accountID.value();
		}();

		return id >= 1 ? std::optional(id) : std::nullopt;
	}

	geode::Result<matjson::Object, std::string> getJsonFromAccID()
	{
		auto path = getFilePathAccID();
		std::ifstream file(path);
		geode::log::info("Try Read {}", path.string());

		if(!file.good())
		{
			geode::log::error("Could not read {}", path.string());
			return Err("Could not read file");
		}
		std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		log::info("str: {}", str);
		std::string jsonError;
		auto jsonopt = matjson::Value::from_str(str, jsonError);
		if(!jsonopt) return geode::Err(jsonError.c_str());
		

		const matjson::Value& json = *jsonopt;
		if(!json.is_object()) return Err("Wrong main json type");
		return Ok(json.as_object());
	}

	void tryLoadLevel(PlayLayer* pl, GJGameLevel* level)
	{
		auto optAccId = getAccIdFromLevel(level);
		if(!optAccId) return error("{}", "Could not get account id of the level");

		m.hasLoadedOnce = true;
		m.mod_enabled = false;
		auto textObjects = getAllTexts(pl);
		for(const auto& str : textObjects)
		{
			if(str == "@perspickup")
			{
				m.mod_enabled = true;
				break;
			}
		}
		if(!m.mod_enabled) return log::error("text @perspickup not found");

		m.accID = *optAccId;
		loadSpecialGroupIds(textObjects);
		loadFromFile(textObjects);
	}

	void saveFileAndReset()
	{
		saveToFile();
		resetMembers();
	}
	void resetMembers()
	{
		m.accID = INVALID_INT;
		m.restoreGroup = INVALID_INT;
		m.saveGroup = INVALID_INT;
		m.applyGroup = INVALID_INT;
		m.enabledGroup = INVALID_INT;
		m.levelSpecifiedPersItemIds.clear();
		m.levelSpecifiedPersItemIds.shrink_to_fit();
		m.temp_died.clear();
		m.hasLoadedOnce = false;
		m.mod_enabled = false;
		log::info("Reseted, size of item ids {}", m.levelSpecifiedPersItemIds.size());
	}

	static PersPickup& get()
	{
		static PersPickup instance;
		return instance;
	}

	void loadSpecialGroupIds(std::span<std::string_view> textObjects)
	{
    	auto check_line = [&](const auto& line)
		{
			if(auto v = getNumericValueFromText(line, "save"))
			{
				m.saveGroup = *v;
			}
			else if(auto v = getNumericValueFromText(line, "enable"))
			{
				m.enabledGroup = *v;
				updateCountForItem(*v, 1);
				if(auto p = playlayer()) p->updateCounters(*v, 1);
			}
    	};

		std::for_each(textObjects.begin(), textObjects.end(), check_line);
	}

	static std::vector<std::string_view> getAllTexts(PlayLayer* pl)
	{
		decltype(getAllTexts(pl)) ret;

		for(const auto& obj : GameObjArr(pl->m_objects))
		{
			if(obj->m_objectID != 914) continue;
			ret.emplace_back(static_cast<TextGameObject*>(obj)->m_text);
		}
		return ret;
	}

	[[nodiscard]] fs::path getFilePathAccID() 
	{
		if(m.accID == INVALID_INT) return {};
		return geode::dirs::getModsSaveDir() /= fmt::format("{}-itemids.json", m.accID);
	}

	void playerBeforeDying()
	{
		for(const auto& ids : m.levelSpecifiedPersItemIds)
		{
			auto currentId = countForItem(ids.itemID);
			if(currentId) m.temp_died.insert_or_assign(ids.itemID, *currentId);
		}
	}

	void playerDied()
	{
		geode::log::info("Player died, setting up ids");
		auto pl = playlayer();
		if(m.enabledGroup != INVALID_INT)
		{
			updateCountForItem(m.enabledGroup, 1, pl);
			pl->updateCounters(m.enabledGroup, 1);
		}

		for(const auto& persid : m.levelSpecifiedPersItemIds)
		{
			auto savedID = m.temp_died.find(persid.itemID);
			if(savedID == m.temp_died.end()) continue;

			int id = savedID->first;
			int val = savedID->second;

			updateCountForItem(id, val, pl);
			pl->updateCounters(id, val);
		}
		m.temp_died.clear();
	}

	[[nodiscard]] bool levelItemIDExists(const PersistentID& other)
	{
		return levelItemIDExists(other.key, other.itemID);
	}

	[[nodiscard]] bool levelItemIDExists(std::string_view key, int itemid)
	{
		for(const auto& checkDuplicate : m.levelSpecifiedPersItemIds)
			if(checkDuplicate.itemID == itemid && checkDuplicate.key == key)
				return true;
		return false;
	}

	static std::vector<PersistentID> getPersistentLevelIds(std::span<std::string_view> textObjects)
	{
		std::vector<PersistentID> ret;
		auto levelItemIDExists = [&](const PersistentID& id)
		{
			for(const auto& checkDuplicate : ret)
				if(checkDuplicate.itemID == id.itemID && checkDuplicate.key == id.key)
					return true;
			return false;
		};


		for(const auto& str : textObjects)
		{
			if(auto opt = PersistentID::fromLevelText(str); opt && !levelItemIDExists(*opt))
			{
				log::info("Emplacing {} {}", opt->key, opt->itemID);
				ret.emplace_back(*opt);
			}
		}
		return ret;
	}

	void loadFromFile(std::span<std::string_view> textObjects)
	{
		m.levelSpecifiedPersItemIds = getPersistentLevelIds(textObjects);

		auto jsonopt = getJsonFromAccID();
		if(!jsonopt) return geode::log::error("{}", jsonopt.unwrapErr());

		const auto& jsonobj = jsonopt.unwrap();

		auto valueForItemId_find = [&jsonobj](const PersistentID& id) -> std::optional<int> {
			if(auto it = jsonobj.find(id.key); it != jsonobj.end())
			{
				return it->second.is_number() ? std::optional(it->second.as_int()) : std::nullopt;
			}
			return {};
		};

		auto pl = playlayer();

		if(m.enabledGroup != INVALID_INT) updateCountForItem(m.enabledGroup, 1, pl);

		for(const auto& item_in_level : m.levelSpecifiedPersItemIds)
		{
			log::info("by {}", item_in_level.key);
			if(auto itemidvalue = valueForItemId_find(item_in_level))
			{
				int itemid = item_in_level.itemID;
				int value = *itemidvalue;
				updateCountForItem(itemid, value, pl);
				pl->updateCounters(itemid, value);
			}
		}
	}

	void saveToFile()
	{
		auto jsonOpt = getJsonFromAccID();
		if(!jsonOpt) log::error("Error parsing json {}", jsonOpt.unwrapErr());


		auto holder = jsonOpt.value_or(matjson::Object{});

		for(int itemIDValue = INVALID_INT; const auto& persistentId : m.levelSpecifiedPersItemIds)
		{
			if(auto it = countForItem(persistentId.itemID); !it) continue;
			else itemIDValue = *it;

			//if duplicated, update the value and continue
			if(auto it = holder.find(persistentId.key); it != holder.end())
			{
				it->second = itemIDValue;
			}
			else
			{
				holder.insert({persistentId.key, itemIDValue});
			}


		}
		
		if(holder.empty())
		{
			return geode::log::error("Could not find any values to save, something wrong probably happened");
		}

		auto path = getFilePathAccID();
		std::ofstream f(path);
		if(!f.good()) return;
		
		auto jsonstr = matjson::Value(holder).dump(4);
		f << jsonstr;
		geode::log::info("Saving: {}", jsonstr);
		geode::log::info("At \"{}\"", path.string());
	}
};


struct MyPlayLayer : Modify<MyPlayLayer, PlayLayer>
{
	void setupHasCompleted()
	{
		PlayLayer::setupHasCompleted();

		PersPickup::get().tryLoadLevel(this, m_level);
	}

#if defined(GEODE_IS_WINDOWS)
	void pauseGame(bool a)
	{
		PlayLayer::pauseGame(a);
		for(const auto& o : from_offset<gd::unordered_map<int, int>>(m_effectManager, 512))
		{
			geode::log::info("{} {}", o.first, o.second);
		}
	}
#endif
	void onQuit()
	{
		PlayLayer::onQuit();
		if(auto p = PersPickup::get(); p.m.mod_enabled)
		{
			p.saveFileAndReset();
		}
	}

	void resetLevel()
	{
		if(auto& p = PersPickup::get(); p.m.hasLoadedOnce && p.m.mod_enabled)
		{
			p.playerBeforeDying();
			PlayLayer::resetLevel();
			p.playerDied();
		}
		else
		{
			PlayLayer::resetLevel();
		}
	}
};

struct PauseLayerHook : geode::Modify<PauseLayerHook, PauseLayer>
{
	void onEdit(CCObject* s)
	{
		if(auto p = PersPickup::get(); p.m.mod_enabled)
		{
			p.saveFileAndReset();
		}
		PauseLayer::onEdit(s);
	} 
};

struct GB : geode::Modify<GB, GJBaseGameLayer>
{
	void spawnGroupTriggered(int groupid, float b, bool c, gd::vector<int> const& d, int e, int f)
	{
		GJBaseGameLayer::spawnGroupTriggered(groupid, b, c, d, e, f);
		if(auto p = PersPickup::get(); playlayer() && p.m.mod_enabled && p.m.hasLoadedOnce && p.m.saveGroup == groupid)
		{
			p.saveToFile();
		}
	}
};